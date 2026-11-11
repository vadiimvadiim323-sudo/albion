#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "basic_winapi_aim.hpp"
#include "config_document.hpp"
#include "performance_stats.hpp"
#include <cstdio>
#include <cstdlib>
#include <cmath>
#include <limits>
template<class T> concept HasPrediction = requires(T t){t.predictionEnabled;};
static_assert(!HasPrediction<BasicAimConfig>);
static double clockNow=0;
static double TestClock(){return clockNow;}
static int checks=0;
static void Check(bool ok,const char* message){++checks;if(!ok){std::fprintf(stderr,"FAIL %s\n",message);std::exit(2);}}
int main(){
 using namespace albion::performance;
 clockNow=0;Telemetry t(TestClock);Check(t.StartBenchmark(),"start benchmark");Check(!t.StartBenchmark(),"no overlapping benchmark");
 t.MenuState(3);clockNow=.01;t.Capture(.3);t.Result(2,3,-1,.1,.2,0,11,1,2,3,4,5,1);t.Delivery(.07);
 clockNow=1.01;t.Capture(.5);t.Result(4,5,-1,.2,.4,2,12,2,3,4,5,6,1);
 clockNow=2.01;auto s=t.Read();
 Check(s.benchmark[InferenceFps].samples==2,"count completed inferences");
 Check(std::abs(s.benchmark[BackendMs].average-3)<1e-10,"sample-weighted backend mean");
 Check(s.benchmark[GpuMs].samples==0 && s.benchmark[GpuMs].average<0,"unavailable GPU is not zero");
 Check(s.benchmark[Objects].samples==2 && s.benchmark[Objects].average==1,"zero objects included");
 clockNow=60;t.Capture(.1);t.Result(900,900,-1,1,1,9);s=t.Read();
 Check(s.finished&&!s.benchmarking,"exact benchmark boundary");
 Check(s.benchmark[InferenceFps].samples==2,"exclude event at end boundary");
 Check(std::abs(s.benchmark[InferenceFps].average-2.0/60)<1e-12,"FPS counts / full duration");
 Check(s.benchmark[InferenceFps].minimum==0,"idle seconds count toward min");
 Check(s.benchmark[BackendMs].maximum==4,"frozen timings exclude later result");
 clockNow=95;s=t.Read();Check(s.benchmarkSeconds==60 && s.benchmark[BackendMs].maximum==4,"frozen benchmark stable");
 t.ExportCompleted();t.ExportCompleted();
 clockNow=100;Check(t.StartBenchmark(),"restart benchmark");s=t.Read();Check(s.benchmark[BackendMs].samples==0,"restart clears data");
 t.Result(std::numeric_limits<double>::quiet_NaN(),-1,-1,-1,-1,0);s=t.Read();Check(s.benchmark[BackendMs].samples==0,"reject NaN timing");
 albion::ConfigDocument cfg;
 Check(cfg.Parse("version=1\nradius=320\nconfidence=0.5\nsmoothing=1.8\nmodel=test.onnx\npredictionEnabled=1\npredictionStrength=0.6\nleadSeconds=0.03\n"),"old profile keys remain importable");
 BasicAimConfig aim;CaptureRegion region{0,0,640,640};std::vector<Detection> dets{{300,280,40,80,.9f,0}};
 auto target=SelectNearestTarget(dets,region,aim);Check(target.has_value(),"target selection preserved");
 Check(target->x==320 && std::abs(target->y-316)<.001,"observed point unchanged");
 Check(AimTimeAlpha(1,.01f)==1,"smoothing helper preserved");
 DevShutdownLog();std::printf("STABLE_CORE_PASS checks=%d no_capture_no_auth_no_mouse_input\n",checks);return 0;
}
