// modules/tracking/tracking.cpp
#include "basic_winapi_aim.hpp"

AimMover::AimMover() noexcept
{
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        mFrequency = double(freq.QuadPart) > 0.0 ? double(freq.QuadPart) : 1.0;
    }

AimMover::~AimMover() noexcept
{ Stop(); }

bool AimMover::Start() noexcept
{
        if (mRunning.load()) return true;
        if (mThread.joinable()) mThread.join();
        mRunning.store(true);
        try {
            mThread = std::thread([this] {
                albion::GuardWorker([this] { Loop(); }, [](const char* e) { DevLog("WORKER", e); },
                    [this] { mRunning = false; });
            });
            return true;
        } catch (...) { mRunning = false; return false; }
    }

void AimMover::Stop() noexcept
{
        { std::lock_guard<std::mutex> lock(mMx); mRunning.store(false); }
        mWake.notify_all();
        if (mThread.joinable()) mThread.join();
    }

void AimMover::Publish(const AimMoverState& state) noexcept
{
        // FIX-V11-IDLE: lazy start; no 1 kHz worker exists in the default mode.
        if (!Start()) return;
        try {
            { std::lock_guard<std::mutex> lock(mMx); mState = state; mHasState = true; TrimSendsLocked(state.captureTime); }
            mWake.notify_one();
        } catch (...) { SetActive(false); }
    }

void AimMover::SetActive(bool active) noexcept
{
        std::lock_guard<std::mutex> lock(mMx);
        if (active) {
            mState.active = true;
            return;
        }
        mHasState = false;
        mState.active = false;
        mSends.clear();
        mSentX = 0.0f;
        mSentY = 0.0f;
        mResidualX = 0.0f;
        mResidualY = 0.0f;
        mCountsX = 0.0f;
        mCountsY = 0.0f;
        mInDeadZone.store(false, std::memory_order_relaxed);
    }

AimMoverCounts AimMover::TakeCounts() noexcept
{
        std::lock_guard<std::mutex> lock(mMx);
        AimMoverCounts counts{ mCountsX, mCountsY };
        mCountsX = 0.0f;
        mCountsY = 0.0f;
        return counts;
    }

bool AimMover::InDeadZone() const noexcept
{
        return mInDeadZone.load(std::memory_order_relaxed);
    }

float AimMover::LeadPixels() const noexcept
{
        return mLeadPixels.load(std::memory_order_relaxed);
    }

float AimMover::StepsPerSecond() const noexcept
{
        return mStepsPerSecond.load(std::memory_order_relaxed);
    }

double AimMover::NowSeconds() const noexcept
{
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return double(now.QuadPart) / mFrequency;
    }

void AimMover::TrimSendsLocked(double cutoff)
{
        float sx = 0.0f;
        float sy = 0.0f;
        std::vector<SendRecord> kept;
        kept.reserve(mSends.size());
        for (const SendRecord& rec : mSends) {
            if (rec.time > cutoff) {
                kept.push_back(rec);
                sx += rec.px;
                sy += rec.py;
            }
        }
        mSends.swap(kept);
        mSentX = sx;
        mSentY = sy;
    }

void AimMover::Loop()
{
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_ABOVE_NORMAL);

        HANDLE timer = CreateWaitableTimerExW(
            nullptr, nullptr, kHighResTimerFlag, TIMER_ALL_ACCESS);
        if (timer == nullptr) {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
        }

        albion::Handle timerLease(timer); // closes on both normal exit and exception
        mLastTick = NowSeconds();
        double rateWindowStart = mLastTick;
        int rateCounter = 0;

        while (mRunning.load(std::memory_order_relaxed)) {
            {
                std::unique_lock<std::mutex> lock(mMx);
                if (!mHasState || !mState.active) {
                    mStepsPerSecond.store(0);
                    mWake.wait(lock, [this] { return !mRunning.load() || (mHasState && mState.active); });
                    if (!mRunning.load()) return;
                    mLastTick = NowSeconds(); rateWindowStart = mLastTick; rateCounter = 0;
                }
            }
            Tick();

            ++rateCounter;
            const double now = NowSeconds();
            if (now - rateWindowStart >= 0.5) {
                mStepsPerSecond.store(
                    float(double(rateCounter) / (now - rateWindowStart)),
                    std::memory_order_relaxed);
                rateWindowStart = now;
                rateCounter = 0;
            }

            WaitTick(timer);
        }

    }

void AimMover::WaitTick(HANDLE timer) noexcept
{
        const double hz = std::clamp(
            double(mHz.load(std::memory_order_relaxed)), 100.0, 4000.0);

        if (timer != nullptr) {
            LARGE_INTEGER due{};
            due.QuadPart = -LONGLONG(10000000.0 / hz);
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                WaitForSingleObject(timer, 50);
                return;
            }
        }

        Sleep(1);
    }

void AimMover::Tick()
{
        LONG moveX = 0;
        LONG moveY = 0;

        {
            std::lock_guard<std::mutex> lock(mMx);

            const double now = NowSeconds();
            const double dt = now - mLastTick;
            mLastTick = now;

            if (!mHasState || !mState.active) {
                mResidualX = 0.0f;
                mResidualY = 0.0f;
                return;
            }

            const BasicAimConfig& cfg = mState.cfg;
            mHz.store(cfg.moverHz, std::memory_order_relaxed);

            // Detekciya protukhla - luchshe stoyat, chem ekstrapolirovat v pustotu.
            const double stale = double(std::max(0.05f, cfg.graceSeconds));
            if (now - mState.detTime > stale) {
                mHasState = false; // sleep until a fresh publish instead of polling stale work
                mResidualX = 0.0f;
                mResidualY = 0.0f;
                return;
            }

            // Use the observed point without any velocity extrapolation.
            const float speed = std::sqrt(mState.velX * mState.velX + mState.velY * mState.velY);
            const float offX = 0.0f, offY = 0.0f;
            mLeadPixels.store(0.0f, std::memory_order_relaxed);

            // Vychitaem uzhe otpravlennoe posle zakhvata kadra.
            const float errX = (mState.aimX + offX) - mState.centerX - mSentX;
            const float errY = (mState.aimY + offY) - mState.centerY - mSentY;
            const float dist = std::sqrt(errX * errX + errY * errY);

            // --- Myortvaya zona: krug, szhimayushchiysya na skorosti ---
            float deadZone = std::max(0.0f, cfg.deadZoneRadius);
            deadZone = std::max(deadZone, std::max(0.0f, cfg.deadZone));
            const float velCancel = std::max(1.0f, cfg.deadZoneVelCancel);
            const float shrink = 1.0f - std::clamp(speed / velCancel, 0.0f, 0.9f);
            deadZone *= shrink;

            const float exitK =
                1.0f + (std::clamp(cfg.deadZoneExit, 1.0f, 4.0f) - 1.0f) * shrink;
            const float exitZone = deadZone * exitK;
            const bool wasInZone = mInDeadZone.load(std::memory_order_relaxed);

            if (dist <= (wasInZone ? exitZone : deadZone)) {
                mInDeadZone.store(true, std::memory_order_relaxed);
                mPathStart = 0.0f;
                mResidualX = 0.0f;
                mResidualY = 0.0f;
                return;
            }
            mInDeadZone.store(false, std::memory_order_relaxed);

            // --- Shag: dolya ostavsheysya oshibki za etot tik ---
            // Pri 1000 Gts i settleSeconds = 0.035 eto okolo 2.8% za tik.
            // Summarno poluchaetsya ta zhe eksponenta, chto zamerena po videoroliku,
            // no razmazannaya po millisekundam vmesto ryvkov raz v kadr.
            float portion = 1.0f;
            // "Skorost": tot zhe smysl, chto sensitivity v MouseManager.cs u
            // Aimmy - odna ruchka, naskolko agressivno pritsel dogonyaet tsel.
            // Ranshe cfg.smoothing schitalsya polzunkom, no v asinkhronnom
            // Tick() nikak ne primenyalsya - polzunok "Skorost" byl mertv.
            const float speedGain = std::clamp(cfg.smoothing, 0.05f, 4.0f);
            const float settle = std::max(0.0f, cfg.settleSeconds) / speedGain;
            if (settle > 0.0005f) {
                const float dtStep = float(std::clamp(dt, 0.0002, 0.05));
                portion = 1.0f - std::exp(-dtStep / settle);
            }
            if (dist > mPathStart) mPathStart = dist;
            portion = std::clamp(
                portion * std::clamp(cfg.bangGain, 0.1f, 1.0f)
                        * AimPathGain(cfg.movementPath, dist, mPathStart, speed),
                0.0f, 1.0f);

            // Idyom na kray zony, a ne v samyy tsentr.
            const float reach = std::max(0.0f, dist - deadZone * 0.5f);
            const float k = reach / std::max(0.001f, dist);
            const float px = std::max(0.05f, mState.pxPerCount);

            float stepX = errX * k * portion / px + mResidualX;
            float stepY = errY * k * portion / px + mResidualY;

            const float maxDelta = std::max(1.0f, cfg.maxDelta);
            stepX = std::clamp(stepX, -maxDelta, maxDelta);
            stepY = std::clamp(stepY, -maxDelta, maxDelta);

            moveX = LONG(stepX);
            moveY = LONG(stepY);

            // Drobnyy ostatok kopim: pri 1000 Gts shag chasto menshe odnogo
            // schyotchika, i bez ostatka dvizhenie prosto propalo by.
            mResidualX = std::clamp(stepX - float(moveX), -1.0f, 1.0f);
            mResidualY = std::clamp(stepY - float(moveY), -1.0f, 1.0f);

            if (moveX != 0 || moveY != 0) {
                const float sentPxX = float(moveX) * px;
                const float sentPxY = float(moveY) * px;
                mSentX += sentPxX;
                mSentY += sentPxY;
                mCountsX += float(moveX);
                mCountsY += float(moveY);
                mSends.push_back(SendRecord{ now, sentPxX, sentPxY });
                if (mSends.size() > 4096) {
                    TrimSendsLocked(now - 0.5);
                }
            }
        }

        // Otpravlyaem uzhe bez blokirovki: zapis v Arduino mozhet zanyat vremya.
        if (moveX != 0 || moveY != 0) {
            SendMouseMove(moveX, moveY);
        }
    }

BasicWinApiAim::BasicWinApiAim(BasicAimConfig config) noexcept
        : mConfig(config)
{
        LARGE_INTEGER freq{};
        QueryPerformanceFrequency(&freq);
        mFrequency = double(freq.QuadPart) > 0.0 ? double(freq.QuadPart) : 1.0;
        // Worker starts only when its optional mode publishes actual work.
    }

void BasicWinApiAim::Shutdown() noexcept
{ mMover.Stop(); }

BasicAimConfig& BasicWinApiAim::Config() noexcept
{ return mConfig; }

const BasicAimConfig& BasicWinApiAim::Config() const noexcept
{ return mConfig; }

float BasicWinApiAim::TargetSpeed() const noexcept
{
        return std::sqrt(mVelX * mVelX + mVelY * mVelY);
    }

float BasicWinApiAim::LeadPixels() const noexcept
{ return mLastLeadPixels; }

float BasicWinApiAim::PixelsPerCount() const noexcept
{ return mPxPerCount; }

bool BasicWinApiAim::InDeadZone() const noexcept
{ return mInDeadZone; }

float BasicWinApiAim::MoverStepsPerSecond() const noexcept
{
        return mMover.StepsPerSecond();
    }

void BasicWinApiAim::Reset() noexcept
{

        mHasLock = false;
        mLockX = 0.0f;
        mLockY = 0.0f;
        mHasSmooth = false;
        mLastRawX = mLastRawY = 0.0f;
        mSmoothX = 0.0f;
        mSmoothY = 0.0f;
        mVelX = 0.0f;
        mVelY = 0.0f;
        mHasVelocity = false;
        mResidualX = 0.0f;
        mResidualY = 0.0f;
        mLastLeadPixels = 0.0f;
        mInDeadZone = false;
        mHasSent = false;
        mSentX = 0.0f;
        mSentY = 0.0f;
        mPrevErrX = 0.0f;
        mPrevErrY = 0.0f;
        mHasStepTime = false;
        mHasDetTime = false;
        mCachedTarget.reset();
        mMover.SetActive(false);
    }

std::optional<AimTarget> BasicWinApiAim::Update(
        const std::vector<Detection>& detections,
        const CaptureRegion& region,
        bool enabled,
        bool newDetection,
        double captureTimeSeconds,
        AimTraceObservation* trace) noexcept
{

        if (trace) *trace = AimTraceObservation{};
        if (!enabled || !IsKeyHeld(mConfig.activationKey)) {
            Reset();
            return std::nullopt;
        }

        if (trace) trace->active = true;
        const double now = NowSeconds();
        // No extrapolation between fresh detections.
        // Expire a stale lock even if the backend stops publishing frames.
        if (mHasDetTime && now - mLastDetTime > std::max(0.0f, mConfig.graceSeconds)) {
            Reset();
        }

        // Между детекциями ничего не отправляем: лишние команды мыши
        // на устаревших данных — это и есть дрожание.
        if (!newDetection) {
            return mCachedTarget;
        }

        std::optional<AimTarget> closest = SelectLockedTarget(
            detections, region, mConfig, mHasLock, mLockX, mLockY);

        if (!closest) {

            // Грейс-период: короткая пропажа детекции не сбрасывает лок,
            // но и мышь не двигаем.
            if (mHasDetTime &&
                float(now - mLastDetTime) > std::max(0.0f, mConfig.graceSeconds)) {
                Reset();
            }
            mCachedTarget.reset();
            return std::nullopt;
        }

        if (trace) { trace->found = true; trace->selected = *closest;
            trace->asyncMode = mConfig.asyncMover && !mConfig.hardMode; }

        // --- Оценка скорости цели в пикселях в секунду ---
        if (mHasLock && mHasDetTime) {
            const float dt = float(std::clamp(now - mLastDetTime, 0.001, 0.20));
            const float jumpX = closest->x - mLockX;
            const float jumpY = closest->y - mLockY;
            const float jump = std::sqrt(jumpX * jumpX + jumpY * jumpY);

            if (jump <= std::max(20.0f, mConfig.matchRadius)) {
                const auto filtered = FilterAimObservation(closest->x, closest->y,
                    mLastRawX, mLastRawY, mSmoothX, mSmoothY, mVelX, mVelY, mHasVelocity,
                    dt, mConfig.velocityJitterPx, mConfig.targetSmoothAlpha, mConfig.velocityAlpha);
                mSmoothX = filtered.x; mSmoothY = filtered.y;
                mVelX = filtered.vx; mVelY = filtered.vy;
                mHasVelocity = true;

            }
            else {
                // Перескочили на другую цель — скорость считаем заново.
                mVelX = 0.0f;
                mVelY = 0.0f;
                mHasVelocity = false;
                // Novaya tsel' -- ne tyanem staruyu sglazhennuyu poziciyu,
                // srazu snapim na svezhuyu detekciyu.
                mSmoothX = closest->x;
                mSmoothY = closest->y;
            }
        }

        if (!mHasSmooth) {
            mSmoothX = closest->x;
            mSmoothY = closest->y;
            mHasSmooth = true;
        }

        // Hard mode uses the current detection point, not the lagging position EMA.
        if (mConfig.hardMode) { mSmoothX = closest->x; mSmoothY = closest->y; }
        mLastRawX = closest->x; mLastRawY = closest->y;
        mLockX = mSmoothX;
        mLockY = mSmoothY;
        mHasLock = true;
        mLastDetTime = now;
        mHasDetTime = true;

        // --- Asinkhronnyy rezhim: otdayom tsel potoku dvizheniya ---
        // Sami mysh ne trogaem voobshche. Potok ispolzuet poluchennuyu tochku i
        // dvigaet pritsel s chastotoy moverHz, ne dozhidayas sleduyushchego kadra.
        if (mConfig.asyncMover && !mConfig.hardMode) {
            const float rawErrX = mSmoothX - region.CenterX();
            const float rawErrY = mSmoothY - region.CenterY();

            // Avtozamer masshtaba po komandam, otpravlennym potokom.
            const AimMoverCounts sent = mMover.TakeCounts();
            // Tsel dvizhetsya - izmerenie oshibki mezhdu kadrami soderzhit
            // ne tolko nashe dvizhenie, no i smeshchenie samoy celi, i
            // schyotchik uedet v storonu. Doveryaem zameru tolko na
            // pochti nepodvizhnoy celi.
            const float calibSpeed = std::sqrt(mVelX * mVelX + mVelY * mVelY);
            const bool calibOk = calibSpeed <= std::max(1.0f, mConfig.calibMaxTargetSpeed);
            if (mConfig.autoSensitivity && mHasSent && calibOk) {
                const float sentMag = std::sqrt(sent.x * sent.x + sent.y * sent.y);
                if (sentMag > 6.0f) {
                    const float moved =
                        ((mPrevErrX - rawErrX) * sent.x + (mPrevErrY - rawErrY) * sent.y) / sentMag;
                    const float est = moved / sentMag;
                    if (est > 0.1f && est < 8.0f) {
                        mPxPerCount += 0.15f * (est - mPxPerCount);
                        mPxPerCount = std::clamp(mPxPerCount, 0.2f, 6.0f);
                    }
                }
            }
            mPrevErrX = rawErrX;
            mPrevErrY = rawErrY;
            mHasSent = true;

            AimMoverState state;
            state.active = true;
            state.aimX = mSmoothX;
            state.aimY = mSmoothY;
            state.velX = mVelX;
            state.velY = mVelY;
            state.centerX = region.CenterX();
            state.centerY = region.CenterY();
            state.detTime = now;
            state.captureTime =
                now - double(std::clamp(mConfig.latencySeconds, 0.0f, 0.15f));
            state.pxPerCount = mPxPerCount;
            state.cfg = mConfig;
            mMover.Publish(state);

            mInDeadZone = mMover.InDeadZone();
            mLastLeadPixels = mMover.LeadPixels();
            mCachedTarget = MakeResult(
                *closest, std::sqrt(rawErrX * rawErrX + rawErrY * rawErrY));
            return mCachedTarget;
        }

        const float speedNow = std::sqrt(mVelX * mVelX + mVelY * mVelY);
        mLastLeadPixels=0;
        float dx = mSmoothX - region.CenterX();
        float dy = mSmoothY - region.CenterY();

        if (trace) { trace->processedX=mSmoothX; trace->processedY=mSmoothY;
            trace->leadX=0; trace->leadY=0;
            trace->errX = dx; trace->errY = dy;
            trace->residualBeforeX = mResidualX; trace->residualBeforeY = mResidualY;
            trace->pxPerCount = mPxPerCount; }
        const float distToTarget = std::sqrt(dx * dx + dy * dy);
        const float maxDelta = std::max(1.0f, mConfig.maxDelta);

        // --- Avtozamer chuvstvitelnosti ---
        // Skolko pikseley na ekrane dayot odin schyotchik myshi. Bez etoy tsifry
        // rezkiy shag libo nedolyotaet, libo pereletaet i nachinaet kolebatsya.
        const bool calibOk = speedNow <= std::max(1.0f, mConfig.calibMaxTargetSpeed);
        if (mConfig.autoSensitivity && mHasSent && calibOk) {
            const float sentMag = std::sqrt(mSentX * mSentX + mSentY * mSentY);
            if (sentMag > 6.0f) {
                // Naskolko oshibka umenshilas vdol napravleniya proshlogo shaga.
                const float moved =
                    ((mPrevErrX - dx) * mSentX + (mPrevErrY - dy) * mSentY) / sentMag;
                const float est = moved / sentMag;
                if (est > 0.1f && est < 8.0f) {
                    mPxPerCount += 0.15f * (est - mPxPerCount);
                    mPxPerCount = std::clamp(mPxPerCount, 0.2f, 6.0f);
                }
            }
        }
        mHasSent = false;
        if (trace) trace->pxPerCount = mPxPerCount;

        // --- Myortvaya zona: krug vokrug tochki navedeniya ---
        float deadZone = std::max(0.0f, mConfig.deadZoneRadius);
        deadZone = std::max(deadZone, std::max(0.0f, mConfig.deadZone));


        // Na dvizhushcheysya tseli zonu szhimaem: stoyat na meste, poka tsel
        // uezzhaet, nelzya - imenno eto i lomalo treking.
        const float targetSpeed = std::sqrt(mVelX * mVelX + mVelY * mVelY);
        const float velCancel = std::max(1.0f, mConfig.deadZoneVelCancel);
        const float shrink = 1.0f - std::clamp(targetSpeed / velCancel, 0.0f, 0.9f);
        deadZone *= shrink;

        // Vykhodim iz zony pozzhe, chem vkhodim - inache drozh na granitse.
        // Na bystroy tseli gisterezis tozhe oslablyaem.
        const float exitK = 1.0f + (std::clamp(mConfig.deadZoneExit, 1.0f, 4.0f) - 1.0f) * shrink;
        const float exitZone = deadZone * exitK;

        if (distToTarget <= (mInDeadZone ? exitZone : deadZone)) {
            mInDeadZone = true;
            mResidualX = 0.0f;
            mResidualY = 0.0f;
            if (trace) { trace->commandKnown = true; trace->residualAfterX = 0; trace->residualAfterY = 0; }
            mPrevErrX = dx;
            mPrevErrY = dy;
            mLastStepTime = now;
            mHasStepTime = true;
            mCachedTarget = MakeResult(*closest, distToTarget);
            return mCachedTarget;
        }
        mInDeadZone = false;

        float stepX = 0.0f;
        float stepY = 0.0f;

        if (mConfig.hardMode) {
            // UI speed is encoded as 0.02..2.0. Map it to 1%..100% of
            // current error per fresh detection, preserving existing movement cap.
            const float speed = std::clamp(mConfig.smoothing * 0.5f, 0.01f, 1.0f);
            float gain = speed;
            if (mConfig.smoothTracking) {
                // Smooth the command magnitude, not the detection point; keep
                // fresh-only updates. Cap dt after a gap to avoid a large jump.
                const float dt = mHasStepTime
                    ? float(std::clamp(now - mLastStepTime, 0.001, 0.05))
                    : 1.0f / 60.0f;
                const float tau = 0.005f + 0.045f * (1.0f - speed);
                gain = -std::expm1(-dt / tau);
            }
            const float outScale = std::isfinite(mConfig.trackingOutputScale) ? std::clamp(mConfig.trackingOutputScale, 0.05f, 2.0f) : 1.0f;
            const float scale = gain * outScale / std::max(0.05f, mPxPerCount);
            stepX = dx * scale + mResidualX;
            stepY = dy * scale + mResidualY;
        }
        else if (mConfig.bangBang) {
            // Idyom ne v tsentr tseli, a na kray myortvoy zony: ostatok puti
            // ne nuzhen, a imenno on i daval perelyot s otkatom nazad.
            const float reach = std::max(0.0f, distToTarget - deadZone * 0.5f);
            const float k = reach / std::max(0.001f, distToTarget);
            // Dolya oshibki za shag zavisit ot togo, skolko proshlo vremeni
            // s proshlogo shaga: chem rezhe detekcii, tem krupnee shag.
            // Skorost svedeniya na tsel ostayotsya odinakovoy.
            float portion = 1.0f;
            const float settle = std::max(0.0f, mConfig.settleSeconds);
            if (settle > 0.0005f && mHasStepTime) {
                const float dtStep =
                    float(std::clamp(now - mLastStepTime, 0.001, 0.20));
                portion = 1.0f - std::exp(-dtStep / settle);
            }
            portion = std::clamp(
                portion * std::clamp(mConfig.bangGain, 0.1f, 1.0f), 0.05f, 1.0f);

            const float scale = portion / std::max(0.05f, mPxPerCount);
            stepX = dx * k * scale + mResidualX;
            stepY = dy * k * scale + mResidualY;
        }
        else {
            const float radiusRef = std::max(1.0f, float(mConfig.radius) * 0.5f);
            const float distFactor = std::min(distToTarget / radiusRef, 1.0f);
            const float gain = std::clamp(
                std::max(0.0f, mConfig.smoothing) * (0.7f + 0.3f * distFactor),
                0.05f,
                std::clamp(mConfig.maxGain, 0.1f, 1.0f));
            stepX = dx * gain + mResidualX;
            stepY = dy * gain + mResidualY;
        }

        stepX = std::clamp(stepX, -maxDelta, maxDelta);
        stepY = std::clamp(stepY, -maxDelta, maxDelta);

        const LONG moveX = LONG(stepX);
        const LONG moveY = LONG(stepY);

        // Drobnyy ostatok kopim, inache medlennye dovodki teryayutsya
        // na privedenii k tselomu i poluchaetsya stupenchatoe dvizhenie.
        mResidualX = std::clamp(stepX - float(moveX), -1.0f, 1.0f);
        mResidualY = std::clamp(stepY - float(moveY), -1.0f, 1.0f);

        if (trace) { trace->commandKnown = true; trace->stepX = stepX; trace->stepY = stepY;
            trace->moveX = moveX; trace->moveY = moveY;
            trace->residualAfterX = mResidualX; trace->residualAfterY = mResidualY; }
        mPrevErrX = dx;
        mPrevErrY = dy;
        mSentX = float(moveX);
        mSentY = float(moveY);
        mHasSent = (moveX != 0 || moveY != 0);
        mLastStepTime = now;
        mHasStepTime = true;

        if (moveX != 0 || moveY != 0) {
            if(trace){trace->sendAttempted=true;trace->commandBeginMs=NowSeconds()*1000.0;}
            SendMouseMove(moveX, moveY);
            if(trace)trace->commandEndMs=NowSeconds()*1000.0;
        }

        mCachedTarget = MakeResult(*closest, distToTarget);
        return mCachedTarget;
    }

AimTarget BasicWinApiAim::MakeResult(const AimTarget& src, float distance) const noexcept
{
        AimTarget result{};
        result.x = src.x;
        result.y = src.y;
        result.distance = distance;
        result.detection = src.detection;
        return result;
    }

double BasicWinApiAim::NowSeconds() const noexcept
{
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        return double(now.QuadPart) / mFrequency;
    }
