#pragma once
// Standalone compilation must suppress Win32 min/max before any dependency
// includes windows.h; previously the monolithic entry supplied these guards.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <condition_variable>
#include "worker_guard.hpp"
#include "dev_diagnostics.hpp"
#include "atomic_file.hpp"

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <limits>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

// Detection is an explicit, self-contained module contract.
#include "modules/core/detection.hpp"
//
// Наведение основано на process_frames() из main.py, но с двумя
// исправлениями, которых в Python нет и из-за которых прицел дёргался:
//   1. Скорость цели считается в пикселях в СЕКУНДУ по реальному времени,
//      а не как разница между соседними кадрами. Опережение = задержка
//      конвейера + фиксированный лид, поэтому прицел успевает за целью.
//   2. Команда мыши отправляется только на свежей детекции, а усиление
//      ограничено 1.0. Раньше при 146% сглаживания каждый шаг перелетал
//      цель и следующий шаг возвращал назад — это и была тряска.

struct CaptureRegion {
    LONG left = 0;
    LONG top = 0;
    UINT width = 0;
    UINT height = 0;

    float CenterX() const noexcept { return float(left) + float(width) * 0.5f; }
    float CenterY() const noexcept { return float(top) + float(height) * 0.5f; }
};

// Аналог target_body_part из main.py
// (calculate_target_points_vectorized: 0=Head, 1=Neck, 2=Body, 3=Closest).
enum class AimBodyPart : int {
    Head = 0,     // центр верхних 15% рамки - headYOffsetPixels
    Neck = 1,     // верх бокса + 30% высоты
    Body = 2,     // верх бокса + 45% высоты
    Closest = 3,  // ближайшая к центру из трёх точек
    Custom = 4    // произвольная доля высоты из aimHeight
};

// Traektoriya dovorota - analog nastroyki "Mouse Movement Path" iz Aimmy.
// Profil zadayot, kak raspredelyaetsya skorost po puti do tseli:
// ot rovnoy linii do myagkogo starta s rezkim finishem.
enum class AimMovementPath : int {
    Linear = 0,
    QuadraticBezier = 1,
    CubicBezier = 2,
    Exponential = 3,
    Adaptive = 4,
};

inline const wchar_t* AimMovementPathName(AimMovementPath p) noexcept {
    switch (p) {
    case AimMovementPath::Linear:          return L"Linear";
    case AimMovementPath::QuadraticBezier: return L"Quadratic Bezier";
    case AimMovementPath::CubicBezier:     return L"Cubic Bezier";
    case AimMovementPath::Exponential:     return L"Exponential";
    default:                               return L"Adaptive";
    }
}

// Mnozhitel k dole shaga. dist - ostavsheesya rasstoyanie, startDist - to zhe
// rasstoyanie v moment zakhvata tseli, speed - skorost tseli v px/sek.
inline float AimPathGain(AimMovementPath path, float dist,
                         float startDist, float speed) noexcept {
    if (startDist < 1.0f) return 1.0f;
    // u - dolya ostavshegosya puti, p - uzhe proydennaya dolya.
    const float u = std::clamp(dist / startDist, 0.02f, 1.0f);
    const float p = 1.0f - u;
    switch (path) {
    case AimMovementPath::Linear:
        // Postoyannaya skorost po vsemu puti.
        return std::clamp(1.0f / std::max(u, 0.2f), 1.0f, 5.0f);
    case AimMovementPath::QuadraticBezier:
        // Myagkiy start, uskorenie k tseli.
        return 0.60f + 0.80f * p;
    case AimMovementPath::CubicBezier:
        // Ochen myagkiy start i rezkiy dovorot v kontse.
        return 0.40f + 1.60f * p * p;
    case AimMovementPath::Exponential:
        // Chistaya eksponenta: bystro srazu, zamedlenie u tseli.
        return 1.0f;
    default:
        // Adaptive: dalyokaya ili bystraya tsel - idyom rezche.
        return 1.0f + std::max(std::clamp(dist / 200.0f, 0.0f, 1.0f),
                               std::clamp(speed / 300.0f, 0.0f, 1.0f));
    }
}

// Convert a per-60Hz EMA coefficient into a time-based coefficient.
// Same wall-clock smoothing at different detection rates; alpha=1 stays unfiltered.
inline float AimTimeAlpha(float alpha, float dt) noexcept {
    alpha = std::clamp(alpha, 0.05f, 1.0f);
    if (alpha >= 1.0f) return 1.0f;
    return -std::expm1(std::log1p(-alpha) * 60.0f * std::clamp(dt, 0.001f, 0.20f));
}

struct AimMotionSample {
    float x, y, vx, vy;
};

// Velocity uses RAW consecutive observations, never the lagging filtered point.
inline AimMotionSample FilterAimObservation(float x, float y, float rawPrevX, float rawPrevY,
    float smoothX, float smoothY, float vx, float vy, bool hasVelocity,
    float dt, float jitterPx, float positionAlpha, float velocityAlpha) noexcept
{
    dt = std::clamp(dt, 0.001f, 0.20f);
    const float dx = x - rawPrevX, dy = y - rawPrevY;
    const float jitter = std::max(0.0f, jitterPx);
    const bool moving = dx * dx + dy * dy > jitter * jitter;
    const float ix = moving ? dx / dt : 0.0f, iy = moving ? dy / dt : 0.0f;
    const float va = AimTimeAlpha(velocityAlpha, dt);
    const float pa = AimTimeAlpha(positionAlpha, dt);
    return { smoothX + pa * (x - smoothX), smoothY + pa * (y - smoothY),
        hasVelocity ? vx + va * (ix - vx) : ix,
        hasVelocity ? vy + va * (iy - vy) : iy };
}

inline constexpr int kAimMinHeadYOffsetPixels = -500;
inline constexpr int kAimMaxHeadYOffsetPixels = 500;

struct BasicAimConfig {
    UINT radius = 320;
    float confidence = 0.50f;

    // Используется только при targetBodyPart == AimBodyPart::Custom.
    float aimHeight = 0.30f;

    // Выбор части тела для наведения.
    AimBodyPart targetBodyPart = AimBodyPart::Body;

    // Screen pixels relative to the center of the upper 15%: positive = up, negative = down.
    // Used by Head and the Head candidate in Closest; other modes are unchanged.
    int headYOffsetPixels = 0;

    // = SMOOTHING_FACTOR из main.py (ползунок "Скорость" в окне).
    float smoothing = 1.80f;

    // = DEAD_ZONE_RADIUS из main.py (там 0).
    float deadZone = 0.0f;

    int targetClass = 0;       // COCO: person
    int activationKey = VK_RBUTTON;

    // = SEND_THRESHOLD из main.py
    float sendThreshold = 0.5f;

    // = MAX_DELTA из main.py
    float maxDelta = 127.0f;

    // Оставлено для совместимости, логика больше не привязана к тику.
    float tickSeconds = 0.0f;

    // --- Новые параметры трекинга ---

    // Result age; used by legacy mover bookkeeping, not predictive lead.
    float latencySeconds = 0.0f;

    // Сглаживание оценки скорости цели (0..1). Больше = отзывчивее, шумнее.
    float velocityAlpha = 0.3f;

    // Porog drozhaniya boksa v pikselyakh mezhdu sosednimi detekciyami.
    // Skachok tseli menshe etogo znacheniya schitaetsya shumom raspoznavaniya,
    // a ne dvizheniem, i ne popadaet v otsenku skorosti. Imenno etot shum,
    // podelennyy na malenkiy dt i razmazannyy EMA-filtrom nizhe, zastavlyal
    // pritsel medlenno vrashchatsya ("letat") vokrug pochti nepodvizhnoy tseli.
    float velocityJitterPx = 2.5f;

    // Sglazhivanie syroy pozicii celi (EMA) do togo, kak ona popadyot v
    // raschyot oshibki i skorosti. 1.0 = bez sglazhivaniya (staroe
    // povedenie). Nizhe = plavnee, no s malen'koy zaderzhkoy. Umen'shaet
    // vidimuyu drozh' pricela, kogda sam boks detekcii shumit -- naprimer,
    // na netipichnykh dlya obucheniya modeli celyakh vrode botov/manekenov
    // v tire, gde regressiya ramki menee uverennaya, chem na zhivykh igrokakh.
    float targetSmoothAlpha = 0.65f;

    // Максимальное усиление шага. Выше 1.0 = гарантированный перелёт.
    float maxGain = 1.0f;

    // Сколько секунд держать лок, если детекция пропала на пару кадров.
    float graceSeconds = 0.12f;

    // Радиус сопоставления "та же цель" между детекциями, пикселей.
    float matchRadius = 140.0f;

    // --- Zhyostkoe navedenie (bang-bang) ---

    // true = rezkiy linejnyj shag pryamo na tsel bez sglazhivaniya.
    // false = staraya plavnaya skhema cherez smoothing/maxGain.
    bool bangBang = true;

    // Direct and smooth tracking are enabled by default.
    bool hardMode = true;
    bool smoothTracking = true;
    float trackingOutputScale = 1.0f; // 1.0 = original output.

    // Dolya oshibki, prokhodimaya za odin shag. 1.0 = ves put srazu.
    float bangGain = 1.0f;

    // Profil skorosti dovorota (Mouse Movement Path v Aimmy).
    AimMovementPath movementPath = AimMovementPath::Adaptive;

    // Postoyannaya vremeni dovorota, sek.
    // Zamer po videoroliku chuzhogo softa: ~0.035 s, polnyy dovorot za
    // 3-4 kadra (okolo 100 ms). Shag schitaetsya ot vremeni, a ne ot chisla
    // kadrov, poetomu povedenie odinakovo pri lyubom FPS detekcii.
    // 0 = mgnovennyy pryzhok na tsel za odin shag.
    float settleSeconds = 0.035f;

    // Myortvaya zona - krug fiksirovannogo radiusa vokrug tochki navedeniya,
    // v pikselyakh ekrana. Ot razmera boksa ne zavisit: blizkaya i dalyokaya
    // tsel ostanavlivayut pritsel odinakovo tochno.
    float deadZoneRadius = 0.0f;

    // Skorost tseli, pri kotoroy zona pochti polnostyu ubiraetsya, px/sek.
    // Na dvizhushcheysya tseli zamiranie v zone i est to samoe
    // "khramaet treking": tsel uezzhaet, a pritsel stoit.
    float deadZoneVelCancel = 90.0f;

    // Gisterezis: vykhod iz zony na etom mnozhitele ot radiusa vkhoda.
    // Bez nego pritsel drozhit na samoy granitse zony.
    float deadZoneExit = 1.8f;

    // Avtopodbor masshtaba "pikseley na schyotchik myshi".
    // VYKLYUCHENO PO UMOLCHANIYU: eto samo-podstraivayushchiysya
    // kontur obratnoy svyazi, i na dvizhushcheysya celi on legko
    // uezzhaet v nepravilnuyu storonu (imenno eto davalo oblyot
    // vokrug celi). U Aimmy takogo avtozamera net voobshche -- tam
    // odna ruchka "Sensitivity", kotoruyu podbiraet sam polzovatel.
    // Po umolchaniyu my delayem to zhe samoe: schyotchik == pikselyu,
    // a vsyu chuvstvitelnost zadayot polzunok "Skorost" (smoothing).
    bool autoSensitivity = false;

    // --- Asinkhronnoe dvizhenie ---

    // VYKLYUCHENO PO UMOLCHANIYU. Kommentariy nizhe byl nepraviln:
    // v realnom iskhodnike Aimmy (MouseManager.cs) NET otdelnogo
    // potoka, kotoryy ekstrapoliruet tsel mezhdu kadrami. Tam rovno
    // odna komanda myshi na odnu detekciyu: oshibka do centra ekrana *
    // (1 - Sensitivity), obrezannaya do +-150 px, i vsyo. Nikakogo
    // dogona po skorosti mezhdu kadrami.
    // U nas zhe potok Tick() mezhdu detekciyami uvelichival "age"
    // (vremya s poslednego kadra) i na etu velichinu rastyagival
    // ekstrapolyaciyu (horizon = age + leadSeconds). Dazhe malenkiy
    // ostatochnyy shum skorosti (est na nepodvizhnoy celi iz-za
    // drozhaniya bounding box'a) pri etom rastyagivalsya vo vsyo
    // bolshee smeshchenie, poka ne prishla sleduyushchaya detekciya i
    // ne vernula pritsel nazad -- eto i davalo postoyannuyu tryasku
    // imenno na nepodvizhnykh manekenakh (na realnykh igrokakh
    // nastoyashchaya skorost byla bolshe etogo shuma, poetomu tam
    // bylo zametno menshe). Sinkhronnyy rezhim (false) rabotaet tak
    // zhe, kak Aimmy: odin shag strogo na svezhey detekcii.
    // true = staryy rezhim s otdelnym potokom vysokoy chastoty.
    bool asyncMover = false;

    // Chastota potoka dvizheniya, Gts. 1000 = shag kazhduyu millisekundu.
    float moverHz = 1000.0f;

    // Avtozamer px/schyotchik doveryaem tolko poka tsel pochti nepodvizhna
    // (px/s nizhe etogo poroga). Na dvizhushcheysya celi izmerenie oshibki
    // mezhdu kadrami soderzhit i nashe dvizhenie, i dvizhenie celi srazu,
    // i schyotchik uezzhaet v storonu -- eto i daet perelyot s otkatom
    // ("oblyot"/"plavanie" vokrug celi) na realnykh igrokah.
    float calibMaxTargetSpeed = 40.0f;
};

inline CaptureRegion MakeCenteredRegion(
    UINT desktopW,
    UINT desktopH,
    UINT radius) noexcept {

    const UINT wanted = std::max<UINT>(2u, radius * 2u);
    const UINT width = std::min<UINT>(wanted, desktopW);
    const UINT height = std::min<UINT>(wanted, desktopH);

    CaptureRegion r;
    r.width = width;
    r.height = height;
    r.left = LONG((desktopW - width) / 2u);
    r.top = LONG((desktopH - height) / 2u);
    return r;
}

inline bool PointInsideRegion(
    float x,
    float y,
    const CaptureRegion& r) noexcept {

    return x >= float(r.left) &&
           y >= float(r.top) &&
           x < float(r.left + LONG(r.width)) &&
           y < float(r.top + LONG(r.height));
}

// Kruglaya oblast zakhvata (FOV): tsel schitaetsya validnoy, esli ona
// popadaet vnutr okruzhnosti, vpisannoy v region, a ne vo ves kvadrat.
inline bool PointInsideCircularRegion(
    float x,
    float y,
    const CaptureRegion& r) noexcept {

    const float cx = r.CenterX();
    const float cy = r.CenterY();
    const float radius = 0.5f * (std::min)(float(r.width), float(r.height));
    const float dx = x - cx;
    const float dy = y - cy;
    return (dx * dx + dy * dy) <= (radius * radius);
}

// Доли высоты бокса из main.py.
inline constexpr float kAimHeadFraction = 0.075f; // center of the upper 15%, a single point
inline constexpr float kAimNeckFraction = 0.30f;
inline constexpr float kAimBodyFraction = 0.45f;

inline const wchar_t* AimBodyPartName(AimBodyPart part) noexcept {
    switch (part) {
    case AimBodyPart::Head:    return L"Head";
    case AimBodyPart::Neck:    return L"Neck";
    case AimBodyPart::Body:    return L"Body";
    case AimBodyPart::Closest: return L"Closest";
    default:                   return L"Custom";
    }
}

inline float ComputeHeadAimPointY(const Detection& d, const BasicAimConfig& cfg) noexcept {
    // A single point, not a vertical dead zone. Positive offsets move it upward.
    return d.y + d.h * kAimHeadFraction - float(std::clamp(cfg.headYOffsetPixels,
        kAimMinHeadYOffsetPixels, kAimMaxHeadYOffsetPixels));
}

// Построчный аналог calculate_target_points_vectorized из main.py.
inline float ComputeAimPointY(
    const Detection& d,
    const BasicAimConfig& cfg,
    float centerY) noexcept {

    const float height = d.h;

    switch (cfg.targetBodyPart) {
    case AimBodyPart::Head:
        return ComputeHeadAimPointY(d, cfg);

    case AimBodyPart::Neck:
        return d.y + height * kAimNeckFraction;

    case AimBodyPart::Body:
        return d.y + height * kAimBodyFraction;

    case AimBodyPart::Closest: {
        const float candidates[3] = {
            ComputeHeadAimPointY(d, cfg),
            d.y + height * kAimNeckFraction,
            d.y + height * kAimBodyFraction
        };

        int minIdx = 0;
        float minDist = std::fabs(candidates[0] - centerY);

        for (int j = 1; j < 3; ++j) {
            const float dist = std::fabs(candidates[j] - centerY);
            if (dist < minDist) {
                minDist = dist;
                minIdx = j;
            }
        }

        return candidates[minIdx];
    }

    default:
        return d.y + height * cfg.aimHeight;
    }
}

struct AimTarget {
    float x = 0.0f;
    float y = 0.0f;
    float distance = 0.0f;
    Detection detection{};
};

// Optional per-detection observation. It never changes the controller output.
struct AimTraceObservation {
    bool active = false, found = false, commandKnown = false, asyncMode = false;
    AimTarget selected{};
    float errX = 0, errY = 0, residualBeforeX = 0, residualBeforeY = 0;
    float residualAfterX = 0, residualAfterY = 0, stepX = 0, stepY = 0;
    float pxPerCount = 0;
    LONG moveX = 0, moveY = 0;
    float processedX=0,processedY=0,leadX=0,leadY=0;
    bool sendAttempted=false;
    double commandBeginMs=0,commandEndMs=0;
};

// Аналог calculate_distances_vectorized + выбор ближайшей цели в main.py.
inline std::optional<AimTarget> SelectNearestTarget(
    const std::vector<Detection>& detections,
    const CaptureRegion& region,
    const BasicAimConfig& cfg) noexcept {

    const float centerX = region.CenterX();
    const float centerY = region.CenterY();
    float bestDistanceSq = std::numeric_limits<float>::max();
    std::optional<AimTarget> best;

    for (const Detection& d : detections) {
        if (d.cls != cfg.targetClass ||
            d.score < cfg.confidence ||
            d.w <= 0.0f ||
            d.h <= 0.0f) {
            continue;
        }

        const float targetX = d.x + d.w * 0.5f;
        const float targetY = ComputeAimPointY(d, cfg, centerY);

        if (!PointInsideCircularRegion(targetX, targetY, region)) {
            continue;
        }

        const float dx = targetX - centerX;
        const float dy = targetY - centerY;
        const float distanceSq = dx * dx + dy * dy;

        if (distanceSq < bestDistanceSq) {
            bestDistanceSq = distanceSq;
            best = AimTarget{ targetX, targetY, std::sqrt(distanceSq), d };
        }
    }

    return best;
}

// Аналог выбора цели с локом (TargetLockSelector из main.py): пока старая
// цель видна и не сильно хуже новой — держимся за неё.
inline std::optional<AimTarget> SelectLockedTarget(
    const std::vector<Detection>& detections,
    const CaptureRegion& region,
    const BasicAimConfig& cfg,
    bool hasLock,
    float lockX,
    float lockY) noexcept {

    if (!hasLock) {
        return SelectNearestTarget(detections, region, cfg);
    }

    const float centerX = region.CenterX();
    const float centerY = region.CenterY();
    const float matchRadius = std::max(20.0f, cfg.matchRadius);

    std::optional<AimTarget> nearestToLock;
    float bestLockDistSq = std::numeric_limits<float>::max();

    for (const Detection& d : detections) {
        if (d.cls != cfg.targetClass ||
            d.score < cfg.confidence ||
            d.w <= 0.0f ||
            d.h <= 0.0f) {
            continue;
        }

        const float targetX = d.x + d.w * 0.5f;
        const float targetY = ComputeAimPointY(d, cfg, centerY);

        if (!PointInsideCircularRegion(targetX, targetY, region)) {
            continue;
        }

        const float lx = targetX - lockX;
        const float ly = targetY - lockY;
        const float lockDistSq = lx * lx + ly * ly;

        if (lockDistSq < bestLockDistSq) {
            bestLockDistSq = lockDistSq;
            const float dx = targetX - centerX;
            const float dy = targetY - centerY;
            nearestToLock = AimTarget{
                targetX, targetY, std::sqrt(dx * dx + dy * dy), d };
        }
    }

    if (nearestToLock && bestLockDistSq <= matchRadius * matchRadius) {
        return nearestToLock;
    }

    return SelectNearestTarget(detections, region, cfg);
}

inline bool IsKeyHeld(int virtualKey) noexcept {
    return (GetAsyncKeyState(virtualKey) & 0x8000) != 0;
}

// Аналог win32api.mouse_event(win32con.MOUSEEVENTF_MOVE, dx, dy, 0, 0).
inline void MoveMouseRelativeWinApi(LONG dx, LONG dy) noexcept {
    if (dx == 0 && dy == 0) {
        return;
    }

    INPUT input{};
    input.type = INPUT_MOUSE;
    input.mi.dx = dx;
    input.mi.dy = dy;
    input.mi.dwFlags = MOUSEEVENTF_MOVE;
    SendInput(1, &input, sizeof(input));
}

// Точка подключения драйвера мыши (Arduino HID подставляет сюда себя).
using MouseMoveSink = void (*)(LONG, LONG);
// FIX-V11-OWNER: UI/owner switches cannot race a worker reading the sink.
inline std::atomic<MouseMoveSink> gMouseMoveSink{nullptr};

inline void SendMouseMove(LONG dx, LONG dy) noexcept {
    if (auto sink = gMouseMoveSink.load(std::memory_order_acquire)) {
        try { sink(dx, dy); } catch (...) { /* an optional transport must fail closed */ }
        return;
    }
    MoveMouseRelativeWinApi(dx, dy);
}


// ---------------------------------------------------------------------------
// Potok dvizheniya myshi.
//
// Razobrano po AIManager.cs iz Aimmy: tam tsikl navedeniya zhivyot v svoyom
// potoke (TaskCreationOptions.LongRunning, ThreadPriority.AboveNormal) i na
// kazhdoy iteracii schitaet shag zanovo ot absolyutnoy tochki, bez nakoplennoy
// istorii. My delaem to zhe samoe, no chastota potoka ne privyazana k chastote
// detekcii: mezhdu kadrami ispolzuetsya poslednyaya poluchennaya tochka.
//
// Uchyot uzhe otpravlennogo. Detekciya vsegda opazdyvaet na vozrast kadra,
// poetomu komandy, otpravlennye posle zakhvata kadra, v ney ne otrazheny.
// Khranim zhurnal otpravok so vremenem i vychitaem iz oshibki tolko te, chto
// sdelany posle zakhvata. Bez etogo potok uletel by za tsel za neskolko
// millisekund.
// ---------------------------------------------------------------------------

struct AimMoverState {
    bool active = false;
    float aimX = 0.0f;
    float aimY = 0.0f;
    float velX = 0.0f;
    float velY = 0.0f;
    float centerX = 0.0f;
    float centerY = 0.0f;
    double detTime = 0.0;
    double captureTime = 0.0;
    float pxPerCount = 1.0f;
    BasicAimConfig cfg{};
};

struct AimMoverCounts {
    float x = 0.0f;
    float y = 0.0f;
};

class AimMover {
public:
    AimMover() noexcept;

    ~AimMover() noexcept;

    AimMover(const AimMover&) = delete;
    AimMover& operator=(const AimMover&) = delete;

    bool Start() noexcept;

    void Stop() noexcept;

    // Vyzyvaetsya na kazhdoy svezhey detekcii.
    void Publish(const AimMoverState& state) noexcept;

    void SetActive(bool active) noexcept;

    // Skolko schyotchikov myshi ushlo s proshloy detekcii - nuzhno dlya
    // avtozamera masshtaba "pikseli na schyotchik".
    AimMoverCounts TakeCounts() noexcept;

    bool InDeadZone() const noexcept;

    float LeadPixels() const noexcept;

    // Fakticheskaya chastota shagov potoka - dlya HUD.
    float StepsPerSecond() const noexcept;

private:
    struct SendRecord {
        double time = 0.0;
        float px = 0.0f;
        float py = 0.0f;
    };

    double NowSeconds() const noexcept;

    // Vsyo, chto otpravleno do zakhvata kadra, uzhe vidno v detekcii.
    void TrimSendsLocked(double cutoff);

    void Loop();

    void WaitTick(HANDLE timer) noexcept;

    // Rasstoyanie do tseli v moment zakhvata - baza dlya profilya traektorii.
    float mPathStart = 0.0f;

    void Tick();

    static constexpr DWORD kHighResTimerFlag = 0x00000002;  // HIGH_RESOLUTION

    std::thread mThread;
    std::atomic<bool> mRunning{ false };
    std::atomic<bool> mInDeadZone{ false };
    std::atomic<float> mLeadPixels{ 0.0f };
    std::atomic<float> mStepsPerSecond{ 0.0f };
    std::atomic<float> mHz{ 1000.0f };

    std::mutex mMx;
    std::condition_variable mWake;
    AimMoverState mState{};
    bool mHasState = false;
    std::vector<SendRecord> mSends;
    float mSentX = 0.0f;
    float mSentY = 0.0f;
    float mResidualX = 0.0f;
    float mResidualY = 0.0f;
    float mCountsX = 0.0f;
    float mCountsY = 0.0f;

    double mLastTick = 0.0;
    double mFrequency = 1.0;
};


class BasicWinApiAim {
public:
    explicit BasicWinApiAim(BasicAimConfig config = {}) noexcept;

    void Shutdown() noexcept;

    BasicAimConfig& Config() noexcept;
    const BasicAimConfig& Config() const noexcept;

    // Диагностика для HUD.
    float TargetSpeed() const noexcept;
    float LeadPixels() const noexcept;

    // Skolko ekrannykh pikseley dayot odin schyotchik myshi (avtozamer).
    float PixelsPerCount() const noexcept;
    bool InDeadZone() const noexcept;

    // Fakticheskaya chastota shagov potoka dvizheniya.
    float MoverStepsPerSecond() const noexcept;

    // Аналог блока "Сброс движения при выключении" в main.py.
    void Reset() noexcept;

    // Вызывается каждый кадр; newDetection = true только когда нейросеть
    // выдала свежий результат.
    std::optional<AimTarget> Update(
        const std::vector<Detection>& detections,
        const CaptureRegion& region,
        bool enabled,
        bool newDetection,
        double captureTimeSeconds = -1.0,
        AimTraceObservation* trace = nullptr) noexcept;

private:
    AimTarget MakeResult(const AimTarget& src, float distance) const noexcept;

    double NowSeconds() const noexcept;

    BasicAimConfig mConfig;
    double mFrequency = 1.0;

    bool mHasLock = false;
    float mLockX = 0.0f;
    float mLockY = 0.0f;

    // Filtered position feeds both synchronous and asynchronous movement.
    // Velocity is estimated separately from consecutive RAW observations,
    // never from the positional filter's lag.
    bool mHasSmooth = false;
    float mLastRawX = 0.0f, mLastRawY = 0.0f;
    float mSmoothX = 0.0f;
    float mSmoothY = 0.0f;

    float mVelX = 0.0f;
    float mVelY = 0.0f;
    bool mHasVelocity = false;

    float mResidualX = 0.0f;
    float mResidualY = 0.0f;
    float mLastLeadPixels = 0.0f;

    bool mInDeadZone = false;
    float mPrevErrX = 0.0f;
    float mPrevErrY = 0.0f;
    float mSentX = 0.0f;
    float mSentY = 0.0f;
    bool mHasSent = false;
    float mPxPerCount = 1.0f;
    double mLastStepTime = 0.0;
    bool mHasStepTime = false;

    double mLastDetTime = 0.0;
    bool mHasDetTime = false;

    std::optional<AimTarget> mCachedTarget;

    AimMover mMover;
};
