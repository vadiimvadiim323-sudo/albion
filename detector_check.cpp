#include "modules/inference/detector.hpp"
#include <iostream>
#include <chrono>
static double CheckNowMs(){return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count();}
int wmain(int argc,wchar_t** argv){
 try{
  if(argc!=2)throw std::runtime_error("Usage: detector-check MODEL");
  Detector detector;detector.Start(argv[1]);double until=CheckNowMs()+20000;
  while(!detector.Ready()){if(!detector.Loading()||CheckNowMs()>until)throw std::runtime_error("Model not ready: "+detector.Error());Sleep(1);}
  const auto& variant=AlbionVariant::Get();std::cout<<"conversion="<<variant.converted<<" prediction=removed"<<" backend="<<detector.Backend()<<"\n";
  std::vector<float> tensor(kTensorFloatsMax);std::uint64_t version=0;std::vector<Detection> detections;DetectionTiming timing{};BackendStageTiming stage{};
  const int net=gNetSize.load();std::uint64_t count=detector.CompletedCount();
  for(int frame=0;frame<12;++frame){
   until=CheckNowMs()+10000;while(!detector.WantsFrame()){if(CheckNowMs()>until||!detector.Ready())throw std::runtime_error("Detector did not release frame");Sleep(1);}
   for(size_t i=0;i<TensorFloatsFor(net);++i)tensor[i]=float((i+frame*29)%251)/251.f;
   LetterboxInfo lb{};lb.net=net;lb.ratio=1;lb.frameId=frame+1;lb.capturedMs=CheckNowMs();lb.submittedMs=lb.capturedMs;
   detector.Push(tensor,lb);until=CheckNowMs()+10000;
   while(detector.CompletedCount()==count){if(CheckNowMs()>until||!detector.Ready())throw std::runtime_error("Frame failed: "+detector.Error());Sleep(1);}
   count=detector.CompletedCount();detector.CopyLatestIfChanged(version,detections,timing,stage);
   if(timing.frameId!=std::uint64_t(frame+1))throw std::runtime_error("Stale frame identity");
   for(const auto& d:detections){if(!std::isfinite(d.x)||!std::isfinite(d.y)||!std::isfinite(d.score))throw std::runtime_error("Invalid detection");}
  }
  detector.Stop();std::cout<<"DETECTOR_CHECK_PASS frames=12 stop=PASS mode=synthetic_input_no_capture_no_auth_no_input\n";DevShutdownLog();return 0;
 }catch(const std::exception& e){std::cerr<<"FAILED "<<e.what()<<"\n";DevShutdownLog();return 1;}
}
