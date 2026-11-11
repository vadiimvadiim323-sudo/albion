#pragma once
#include <array>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <deque>
#include <limits>
#include <mutex>
#include <atomic>
#include <vector>
#include <sstream>
#include <iomanip>
#include <locale>
#include "dev_diagnostics.hpp"
#include "atomic_file.hpp"

namespace albion::performance {
inline double Clock() noexcept { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
enum Metric : size_t { CaptureFps, InferenceFps, CaptureMs, BackendMs, GpuMs, PipelineMs, DeliveryMs, DecodeMs, QueueMs, Objects, Count };
struct Aggregate {
    double sum=0, low=std::numeric_limits<double>::infinity(), high=0, last=-1;
    uint64_t count=0;
    void Add(double v) { if(!std::isfinite(v)||v<0)return; sum+=v;low=(std::min)(low,v);high=(std::max)(high,v);last=v;++count; }
    void Merge(const Aggregate& a) { if(!a.count)return;sum+=a.sum;count+=a.count;low=(std::min)(low,a.low);high=(std::max)(high,a.high);last=a.last; }
};
struct View { double current=-1, average=-1, minimum=-1, maximum=-1; uint64_t samples=0; };
struct Snapshot { std::array<View,Count> metrics{};double uptime=0,window=0,benchmarkSeconds=0;bool benchmarking=false,finished=false; std::array<View,Count> benchmark{}; };
struct FrameRecord {
    double elapsed=0, backend=0,pipeline=0,gpu=-1,decode=0,queue=0,objects=0;
    uint64_t frame=0; double image=0,acquired=0,submitted=0,worker=0,completed=0;
    unsigned accumulated=0,menu=0;
};
class Telemetry {
    std::vector<FrameRecord> raw_;
    uint64_t rawDropped_=0;
    bool exported_=false;
    std::atomic<unsigned> menu_{0};

    struct Bucket { int64_t second=0; std::array<Aggregate,Count> data{}; };
    mutable std::mutex mutex_;
    double (*clock_)()=Clock;
    double started_=0, benchStart_=0; bool running_=false,finished_=false;
    std::deque<Bucket> buckets_,benchBuckets_;
    static void Add(std::deque<Bucket>& q,double t,Metric m,double v) {
        if(!std::isfinite(v)||v<0)return;
        const auto sec=int64_t(std::floor(t));
        if(q.empty()||q.back().second!=sec) { Bucket b;b.second=sec;q.push_back(b); }
        q.back().data[m].Add(v);
    }
    void Finish(double now) { if(running_&&now-benchStart_>=60) {running_=false;finished_=true;DevLog("BENCHMARK","Completed full 60-second window");} }
    void Record(Metric m,double v,double now) {
        Finish(now);Add(buckets_,now-started_,m,v);
        while(!buckets_.empty() && buckets_.front().second<int64_t(now-started_)-299) buckets_.pop_front();
        if(running_)Add(benchBuckets_,now-benchStart_,m,v);
    }
    static std::array<View,Count> Summarize(const std::deque<Bucket>& q,double end,double begin) {
        std::array<View,Count> out{};
        for(size_t m=0;m<Count;++m) {
            Aggregate total;double lo=std::numeric_limits<double>::infinity(),hi=0;bool full=false;
            for(const auto& b:q) if(double(b.second)>=begin && double(b.second)<=end) total.Merge(b.data[m]);
            auto& v=out[m];v.samples=total.count;
            if(m==CaptureFps||m==InferenceFps) {
                v.average=end>begin?double(total.count)/(end-begin):0;
                const int64_t previous=int64_t(std::floor(end))-1;
                const int64_t first=int64_t(std::ceil(begin));
                const int64_t complete=(std::max)(int64_t(0),previous-first+1);
                int64_t populated=0;
                if(previous>=begin)v.current=0;
                for(const auto& b:q) {
                    if(b.second==previous)v.current=double(b.data[m].count);
                    if(b.second>=first&&b.second<=previous){const double n=double(b.data[m].count);lo=(std::min)(lo,n);hi=(std::max)(hi,n);++populated;}
                }
                if(populated<complete)lo=0;
                full=complete>0;v.minimum=full?lo:-1;v.maximum=full?hi:-1;
            } else if(total.count) {
                v.average=total.sum/double(total.count);v.minimum=total.low;v.maximum=total.high;
                // Old timings are not presented as a fresh measurement.
                for(auto i=q.rbegin();i!=q.rend();++i) if(i->data[m].count){if(end-double(i->second)<2.0)v.current=i->data[m].last;break;}
            }
        }
        return out;
    }
public:
    explicit Telemetry(double (*clock)()=Clock):clock_(clock),started_(clock()) {}
    void Capture(double ms) {std::lock_guard<std::mutex> l(mutex_);const double now=clock_();Record(CaptureFps,1,now);Record(CaptureMs,ms,now);}
    void Result(double backend,double pipeline,double gpu,double decode,double queue,double objects, uint64_t frame=0,double image=0,double acquired=0,double submitted=0,double worker=0,double completed=0,unsigned accumulated=0) {
        std::lock_guard<std::mutex> l(mutex_);const double now=clock_();
        Finish(now);
        if(running_) {
            if(raw_.size()<65536)raw_.push_back({now-benchStart_,backend,pipeline,gpu,decode,queue,objects,frame,image,acquired,submitted,worker,completed,accumulated,menu_.load(std::memory_order_relaxed)});
            else ++rawDropped_;
        }
        Record(InferenceFps,1,now);Record(BackendMs,backend,now);Record(PipelineMs,pipeline,now);Record(GpuMs,gpu,now);Record(DecodeMs,decode,now);Record(QueueMs,queue,now);Record(Objects,objects,now);
    }
    void Delivery(double ms) {std::lock_guard<std::mutex> l(mutex_);const double now=clock_();Record(DeliveryMs,ms,now);}
    bool StartBenchmark() {std::lock_guard<std::mutex> l(mutex_);if(running_)return false;benchBuckets_.clear();raw_.clear();raw_.reserve(65536);rawDropped_=0;exported_=false;benchStart_=clock_();running_=true;finished_=false;DevLog("BENCHMARK","Started 60-second raw recording; menu_flags: main=1, analytics=2");return true;}
    void MenuState(unsigned flags) noexcept {menu_.store(flags,std::memory_order_relaxed);}
    void ExportCompleted() {
        std::vector<FrameRecord> rows;uint64_t dropped=0;
        {std::lock_guard<std::mutex> lock(mutex_);if(!finished_||exported_)return;exported_=true;rows.swap(raw_);dropped=rawDropped_;}
        if(!DevLogOwner().Post([rows=std::move(rows),dropped] {
            SYSTEMTIME t{};GetLocalTime(&t);wchar_t name[160]{};
            swprintf_s(name,L"performance-60s-%04u%02u%02u-%02u%02u%02u-%03u-p%lu.csv",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,t.wMilliseconds,GetCurrentProcessId());
            std::ostringstream csv;csv.imbue(std::locale::classic());csv<<std::fixed<<std::setprecision(6);
            csv<<"# schema=stable-telemetry-v1; window_seconds=60; stage_clock=QPC_ms; elapsed_clock=steady_relative_s; menu_bits=main:1|analytics:2; gpu_negative=unavailable; raw_dropped="<<dropped<<"\n";
            csv<<"elapsed_s,frame_id,image_qpc_ms,acquired_qpc_ms,submitted_qpc_ms,worker_qpc_ms,completed_qpc_ms,backend_wall_ms,capture_to_result_ms,gpu_compute_ms,decode_ms,submission_queue_ms,objects,dxgi_accumulated_frames,menu_flags\n";
            for(const auto& v:rows)csv<<v.elapsed<<','<<v.frame<<','<<v.image<<','<<v.acquired<<','<<v.submitted<<','<<v.worker<<','<<v.completed<<','<<v.backend<<','<<v.pipeline<<','<<v.gpu<<','<<v.decode<<','<<v.queue<<','<<v.objects<<','<<v.accumulated<<','<<v.menu<<'\n';
            if(!albion::WriteAtomic(AlbionDataPath(name),csv.str())){++gDevLogFailures;DevLog("BENCHMARK","CSV export failed: atomic file write");}
            else DevLog("BENCHMARK",("CSV exported: rows="+std::to_string(rows.size())+" raw_dropped="+std::to_string(dropped)).c_str());
        })) {++gDevLogDropped;DevLog("BENCHMARK","CSV export queue full; raw benchmark export unavailable");}
    }
    Snapshot Read() {
        std::lock_guard<std::mutex> l(mutex_);const double now=clock_();Finish(now);Snapshot s;
        s.uptime=now-started_;const double begin=(std::max)(0.0,std::floor(s.uptime)-299.0);s.window=s.uptime-begin;
        s.metrics=Summarize(buckets_,s.uptime,begin);s.benchmarking=running_;s.finished=finished_;
        if(running_||finished_) {s.benchmarkSeconds=(std::min)(60.0,now-benchStart_);s.benchmark=Summarize(benchBuckets_,s.benchmarkSeconds,0);}
        return s;
    }
};
inline Telemetry telemetry;
}
