#pragma once
// Application-local ONNX Runtime: no dependency on Windows' WinML model parser.
#include <onnxruntime_cxx_api.h>
#pragma push_macro("OPTIONAL")
#include <dml_provider_factory.h>
#pragma pop_macro("OPTIONAL")
#include <climits>
#include <stdexcept>
#pragma comment(lib, "onnxruntime.lib")
class OrtBackend final : public IInferenceBackend {
public:
    bool Init(const std::wstring& path, int hint, std::string& error) {
        try {
            mEnv = Ort::Env(ORT_LOGGING_LEVEL_WARNING, "AlbionORT");
            if (!AimPerfEnvFlag(L"ALBION_ORT_CPU")) {
                try { CreateSession(path, true); mName = "ORT/DirectML"; }
                catch (const Ort::Exception& e) { mGpuError = e.what(); }
            }
            if (!mSession) { CreateSession(path, false); mName = "ORT/CPU"; }
            if (mSession.GetInputCount()!=1 || mSession.GetOutputCount()!=1)
                throw std::runtime_error("Expected exactly one ONNX input and output");
            auto ti=mSession.GetInputTypeInfo(0);
            auto input=ti.GetTensorTypeAndShapeInfo();
            auto shape=input.GetShape();
            if(input.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT || shape.size()!=4)
                throw std::runtime_error("ONNX input must be float32 NCHW [1,3,N,N]");
            if((shape[0]>0 && shape[0]!=1)||(shape[1]>0 && shape[1]!=3))
                throw std::runtime_error("ONNX requires batch=1 and channels=3");
            const int64_t h=shape[2]>0 ? shape[2] : (shape[3]>0 ? shape[3] : hint);
            const int64_t w=shape[3]>0 ? shape[3] : h;
            if(h!=w || h<64 || h>640)
                throw std::runtime_error("ONNX requires square input in 64..640; re-export unsupported input");
            mNetSize=int(h);
            Ort::AllocatorWithDefaultOptions alloc;
            mInName=mSession.GetInputNameAllocated(0,alloc).get();
            mOutName=mSession.GetOutputNameAllocated(0,alloc).get();
            mMemInfo=Ort::MemoryInfo::CreateCpu(OrtArenaAllocator,OrtMemTypeDefault);
            error.clear(); return true;
        } catch(const std::exception& e) {
            error=std::string("ONNX Runtime ")+OrtGetApiBase()->GetVersionString()+": "+e.what();
            if(!mGpuError.empty()) error+="; DirectML attempt: "+mGpuError;
            return false;
        }
    }
    const char* Name() const noexcept override {return mName;}
    int InputSide() const noexcept override {return mNetSize;}
    std::string Diagnostics() const override {
        return std::string(mName)+"; bundled ORT "+OrtGetApiBase()->GetVersionString()+
            "; validated float32 [1,3,"+std::to_string(mNetSize)+","+std::to_string(mNetSize)+"]"+
            (mGpuError.empty()?"":"; CPU fallback because DirectML session failed: "+mGpuError);
    }
    bool RunView(const float* input,std::vector<float>&,InferenceOutputView& view,
        int& channels,int& anchors,std::string& error) override {
        view={}; channels=anchors=0;
        try {
            const int64_t shape[]{1,3,mNetSize,mNetSize};
            auto tensor=Ort::Value::CreateTensor<float>(mMemInfo,const_cast<float*>(input),
                size_t(3)*mNetSize*mNetSize,shape,4);
            const char* ins[]{mInName.c_str()}; const char* outs[]{mOutName.c_str()};
            mOutputs=mSession.Run(Ort::RunOptions{nullptr},ins,&tensor,1,outs,1);
            if(mOutputs.size()!=1 || !mOutputs[0].IsTensor())
                throw std::runtime_error("ONNX output is not a tensor");
            auto info=mOutputs[0].GetTensorTypeAndShapeInfo();auto d=info.GetShape();
            if(info.GetElementType()!=ONNX_TENSOR_ELEMENT_DATA_TYPE_FLOAT ||
                d.size()!=3 || d[0]!=1 || d[1]<=0 || d[2]<=0 || d[1]>INT_MAX || d[2]>INT_MAX)
                throw std::runtime_error("ONNX output must be float32 [1,C,A] or [1,A,C]");
            channels=int(d[1]); anchors=int(d[2]);
            view={mOutputs[0].GetTensorData<float>(),info.GetElementCount()};
            return true;
        } catch(const std::exception& e) {error=std::string(mName)+": "+e.what();return false;}
    }
    bool Run(const float* input,std::vector<float>& out,int& channels,int& anchors,std::string& error) override {
        InferenceOutputView view{};
        if(!RunView(input,out,view,channels,anchors,error)) return false;
        out.assign(view.data,view.data+view.count);return true;
    }
private:
    void CreateSession(const std::wstring& path,bool gpu) {
        // Fresh options for each attempt. Appending an EP can succeed while
        // session creation fails; CPU retry must not retain the failed EP.
        Ort::SessionOptions so;
        so.SetIntraOpNumThreads(1);so.SetInterOpNumThreads(1);
        so.SetExecutionMode(ORT_SEQUENTIAL);
        so.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
        so.AddConfigEntry("session.intra_op.allow_spinning","0");
        if(gpu) {
            so.DisableMemPattern();
            const OrtDmlApi* dml=nullptr;
            Ort::ThrowOnError(Ort::GetApi().GetExecutionProviderApi("DML",ORT_API_VERSION,
                reinterpret_cast<const void**>(&dml)));
            OrtDmlDeviceOptions deviceOptions{OrtDmlPerformancePreference::HighPerformance,OrtDmlDeviceFilter::Gpu};
            Ort::ThrowOnError(dml->SessionOptionsAppendExecutionProvider_DML2(so,&deviceOptions));
        }
        mSession=Ort::Session(mEnv,path.c_str(),so);
    }
    Ort::Env mEnv{nullptr};
    Ort::Session mSession{nullptr};
    Ort::MemoryInfo mMemInfo{nullptr};
    std::vector<Ort::Value> mOutputs; // borrowed output lives until next Run
    std::string mInName,mOutName,mGpuError;
    const char* mName="ORT/CPU";
    int mNetSize=640;
};
