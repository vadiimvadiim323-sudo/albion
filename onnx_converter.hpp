#pragma once
// Local-only TensorRT compiler. No shell, Python, downloads or credential access.
#include <windows.h>
#include <filesystem>
#include <fstream>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <string>
#include <stdexcept>
#include <algorithm>
#include <cwctype>
#include <iterator>

namespace albion {
namespace converter {
namespace fs = std::filesystem;
struct Handle {
    HANDLE h = nullptr;
    Handle() = default;
    explicit Handle(HANDLE value) : h(value) {}
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    ~Handle() { if (h && h != INVALID_HANDLE_VALUE) CloseHandle(h); }
    explicit operator bool() const { return h && h != INVALID_HANDLE_VALUE; }
};
inline std::wstring Quote(const std::wstring& s) {
    std::wstring result=L"\""; size_t slash=0;
    for(wchar_t c:s) {
        if(c==L'\\') { ++slash; continue; }
        if(c==L'\"') result.append(slash*2+1,L'\\');
        else result.append(slash,L'\\');
        slash=0; result+=c;
    }
    result.append(slash*2,L'\\'); result+=L'\"'; return result;
}
inline std::wstring Utf8(const std::string& text) {
    if(text.empty())return {};
    const int n=MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),nullptr,0);
    if(n<=0)return L"Ошибка конвертации";
    std::wstring out(size_t(n),L' ');
    MultiByteToWideChar(CP_UTF8,0,text.data(),int(text.size()),out.data(),n); return out;
}
inline fs::path Unique(const fs::path& folder,const std::wstring& stem,const std::wstring& ext) {
    // Never replace an existing engine, archive, log or work directory.
    for(unsigned i=0;i<10000;++i) {
        auto p=folder/(stem+(i?L"-"+std::to_wstring(i):L"")+ext);
        const bool modelConflict=ext==L".engine" &&
            (fs::exists(fs::path(p.wstring()+L".albion-model")) || fs::exists(folder/L"models"/p.filename()));
        if(!fs::exists(p) && !modelConflict)return p;
    }
    throw std::runtime_error("Too many files with the same name");
}
struct Options {
    fs::path source, app, compiler, validator;
    // User confirms YOLOv8/11 raw semantics in the UI; a supplied sidecar wins.
    std::string defaultContract = "format=yolo8\naxes=channels_first\ncoordinates=pixels\nverified_nms=0\n";
    unsigned optimization=3;
    DWORD buildTimeoutMs=30*60*1000;
};
struct Snapshot {
    bool running=false, done=false, success=false, cancelled=false;
    std::wstring phase, error;
    fs::path engine, archivedOnnx, log;
    ULONGLONG started=0;
};
class Compiler {
public:
    using Validator=std::function<void(const fs::path&)>;
    ~Compiler() { Cancel(); Join(); }
    void Cancel() noexcept { cancel.store(true); }
    void Join() { if(worker.joinable())worker.join(); }
    Snapshot Get() const { std::lock_guard<std::mutex> lock(mx);return state; }
    bool Start(const Options& opt,HANDLE pauseReady,Validator validator) {
        { std::lock_guard<std::mutex> lock(mx); if(state.running)return false; }
        Join();cancel=false;
        { std::lock_guard<std::mutex> lock(mx); state={};state.running=true;
          state.started=GetTickCount64();state.phase=L"Ожидание остановки обработки…"; }
        try { worker=std::thread([this,opt,pauseReady,validator=std::move(validator)] {
            try { Execute(opt,pauseReady,validator); }
            catch(const std::exception& e) { Finish(false,Utf8(e.what())); }
            catch(...) { Finish(false,L"Неизвестная ошибка конвертации"); }
        }); } catch(...) { Finish(false,L"Не удалось создать поток конвертации");return false; }
        return true;
    }
private:
    mutable std::mutex mx;
    Snapshot state;
    std::thread worker;
    std::atomic<bool> cancel{false};
    void Phase(const wchar_t* text) { std::lock_guard<std::mutex> lock(mx);state.phase=text; }
    void Check() const { if(cancel.load())throw std::runtime_error("Conversion cancelled; original files preserved"); }
    void Finish(bool ok,const std::wstring& error={}) {
        std::lock_guard<std::mutex> lock(mx);
        state.success=ok;state.error=error;state.cancelled=!ok&&cancel.load();
        state.phase=ok?L"Готово":(state.cancelled?L"Отменено":L"Ошибка");
        state.running=false;state.done=true;
    }
    void RunChild(const Options& opt,const std::vector<std::wstring>& args,const fs::path& log) {
        SECURITY_ATTRIBUTES sa{sizeof(sa),nullptr,TRUE};
        Handle output(CreateFileW(log.c_str(),GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,
            &sa,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));
        Handle input(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&sa,
            OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!output||!input)throw std::runtime_error("Cannot open conversion log");
        Handle job(CreateJobObjectW(nullptr,nullptr));
        if(!job)throw std::runtime_error("Cannot create converter process job");
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
        limit.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        if(!SetInformationJobObject(job.h,JobObjectExtendedLimitInformation,&limit,sizeof(limit)))
            throw std::runtime_error("Cannot protect converter child process lifetime");
        // Only the log and NUL handles are inherited, not app/license/pipe handles.
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        std::vector<unsigned char> attributes(bytes);
        auto list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attributes.data());
        if(!InitializeProcThreadAttributeList(list,1,0,&bytes))throw std::runtime_error("Cannot create child attributes");
        struct AttributeLease { LPPROC_THREAD_ATTRIBUTE_LIST p;~AttributeLease(){DeleteProcThreadAttributeList(p);} } lease{list};
        HANDLE handles[]={output.h,input.h};
        if(!UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,handles,sizeof(handles),nullptr,nullptr))
            throw std::runtime_error("Cannot restrict child handle inheritance");
        std::wstring command=Quote(opt.compiler.wstring());
        for(const auto& a:args)command+=L" "+Quote(a);
        std::vector<wchar_t> cmd(command.begin(),command.end());cmd.push_back(0);
        STARTUPINFOEXW si{};si.StartupInfo.cb=sizeof(si);
        si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        si.StartupInfo.hStdOutput=output.h;si.StartupInfo.hStdError=output.h;si.StartupInfo.hStdInput=input.h;
        si.lpAttributeList=list;PROCESS_INFORMATION pi{};
        std::vector<std::wstring> environment;
        wchar_t* rawEnv=GetEnvironmentStringsW();
        if(!rawEnv)throw std::runtime_error("Cannot read child environment");
        struct EnvLease {wchar_t* p;~EnvLease(){FreeEnvironmentStringsW(p);}} envLease{rawEnv};
        std::wstring oldPath;
        for(const wchar_t* e=rawEnv;*e;e+=wcslen(e)+1) {
            if(_wcsnicmp(e,L"PATH=",5)==0)oldPath=e+5;
            else environment.emplace_back(e);
        }
        const auto compilerDir=opt.compiler.parent_path();
        environment.push_back(L"PATH="+compilerDir.wstring()+L";"+(compilerDir.parent_path()/L"lib").wstring()+L";"+oldPath);
        std::sort(environment.begin(),environment.end(),[](const auto& a,const auto& b){return _wcsicmp(a.c_str(),b.c_str())<0;});
        std::vector<wchar_t> envBlock;
        for(const auto& e:environment){envBlock.insert(envBlock.end(),e.begin(),e.end());envBlock.push_back(0);}envBlock.push_back(0);
        if(!CreateProcessW(opt.compiler.c_str(),cmd.data(),nullptr,nullptr,TRUE,
            CREATE_NO_WINDOW|CREATE_SUSPENDED|EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT,
            envBlock.data(),opt.app.c_str(),&si.StartupInfo,&pi))
            throw std::runtime_error("Cannot launch bundled TensorRT compiler (Windows error "+std::to_string(GetLastError())+")");
        Handle process(pi.hProcess),thread(pi.hThread);
        if(!AssignProcessToJobObject(job.h,process.h)) {
            TerminateProcess(process.h,1);WaitForSingleObject(process.h,3000);
            throw std::runtime_error("Cannot contain converter process; build was not started");
        }
        if(ResumeThread(thread.h)==DWORD(-1))throw std::runtime_error("Cannot resume converter process");
        const ULONGLONG start=GetTickCount64();
        for(;;) {
            const DWORD result=WaitForSingleObject(process.h,100);
            if(result==WAIT_OBJECT_0)break;
            if(result==WAIT_FAILED)throw std::runtime_error("Cannot monitor converter process");
            if(cancel.load()||GetTickCount64()-start>opt.buildTimeoutMs) {
                TerminateJobObject(job.h,1);WaitForSingleObject(process.h,3000);Check();
                throw std::runtime_error("Converter subprocess exceeded its time limit; see conversion log");
            }
        }
        DWORD code=1;
        if(!GetExitCodeProcess(process.h,&code)||code!=0)
            throw std::runtime_error("TensorRT compilation failed (exit "+std::to_string(code)+"); see conversion log");
        Check();
    }
    void Execute(const Options& options,HANDLE ready,const Validator& validator) {
        Options opt=options;
        opt.app=fs::absolute(opt.app);opt.source=fs::absolute(opt.source);opt.compiler=fs::absolute(opt.compiler);
        if(opt.optimization>5)throw std::runtime_error("Invalid builder optimization level");
        auto ext=opt.source.extension().wstring();std::transform(ext.begin(),ext.end(),ext.begin(),::towlower);
        if(ext!=L".onnx"||!fs::is_regular_file(opt.source)||fs::file_size(opt.source)==0)
            throw std::runtime_error("Select a non-empty ONNX file");
        if(!fs::is_regular_file(opt.compiler))throw std::runtime_error("Bundled extras/converter/trtexec.exe not found");
        if(!validator && !fs::is_regular_file(opt.validator))throw std::runtime_error("Bundled engine validator not found");
        if(!ready)throw std::runtime_error("No safe pipeline pause event");
        const ULONGLONG waitStart=GetTickCount64();
        for(;;) {
            Check();const DWORD result=WaitForSingleObject(ready,100);
            if(result==WAIT_OBJECT_0)break;
            if(result==WAIT_FAILED||GetTickCount64()-waitStart>15000)
                throw std::runtime_error("Processing could not stop safely; converter was not started");
        }
        Check();
        const fs::path archive=opt.app/L"onnx_sources", logs=opt.app/L"conversion_logs";
        fs::create_directories(archive);fs::create_directories(logs);
        const auto stem=opt.source.stem().wstring();
        const auto token=std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64());
        const fs::path work=opt.app/(L".conversion-"+token);
        if(!fs::create_directory(work))throw std::runtime_error("Cannot reserve conversion workspace");
        struct WorkLease { fs::path p;~WorkLease(){std::error_code e;fs::remove_all(p,e);} } workLease{work};
        const fs::path log=Unique(logs,stem+L"-"+token,L".log");
        {std::lock_guard<std::mutex> lock(mx);state.log=log;}
        Phase(L"Сборка FP16 engine · подбор CUDA-ядер…");
        const auto temp=work/L"compiled.engine";
        // Original ONNX stays put until commit; relative external tensor data
        // can still be found by the ONNX parser next to the original file.
        RunChild(opt,{L"--onnx="+opt.source.wstring(),L"--saveEngine="+temp.wstring(),
            L"--fp16",L"--builderOptimizationLevel="+std::to_wstring(opt.optimization),
            L"--maxAuxStreams=0",L"--memPoolSize=workspace:1024",L"--skipInference"},log);
        if(!fs::is_regular_file(temp)||fs::file_size(temp)<64)throw std::runtime_error("Compiler did not produce a usable engine");
        Phase(L"Проверка engine: размеры, запуск, конечные значения…");
        Check();
        if(validator) validator(temp);
        else {
            Options validation=opt;validation.compiler=opt.validator;validation.buildTimeoutMs=120000;
            RunChild(validation,{temp.wstring()},fs::path(log.wstring()+L".validation.log"));
        }
        Check();
        // Archive before publishing the engine. No overwrites, no deletion of
        // externally-selected originals or external ONNX tensor-data files.
        Phase(L"Сохранение engine и исходника ONNX…");
        const fs::path archived=Unique(archive,stem,L".onnx");
        const fs::path destination=Unique(opt.app,stem+L"-fp16",L".engine");
        const fs::path sourceContract=fs::path(opt.source.wstring()+L".albion-model");
        const fs::path destinationContract=fs::path(destination.wstring()+L".albion-model");
        bool archiveCreated=false,contractCreated=false,published=false;
        try {
            // copy_file fails if anything appeared at that path in the meantime.
            fs::copy_file(opt.source,archived,fs::copy_options::none);archiveCreated=true;
            std::string contract=opt.defaultContract;
            if(fs::exists(sourceContract)) {
                if(fs::file_size(sourceContract)>65536)throw std::runtime_error("Model output contract is too large");
                std::ifstream in(sourceContract,std::ios::binary);
                if(!in)throw std::runtime_error("Cannot read source model output contract");
                contract.assign(std::istreambuf_iterator<char>(in),{});
                if(in.bad())throw std::runtime_error("Cannot read source model output contract");
            }
            {
                Handle f(CreateFileW(destinationContract.c_str(),GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
                if(!f)throw std::runtime_error("Cannot save model output contract");
                contractCreated=true;DWORD written=0;
                if(!WriteFile(f.h,contract.data(),DWORD(contract.size()),&written,nullptr)||written!=contract.size()||!FlushFileBuffers(f.h))
                    throw std::runtime_error("Cannot write model output contract");
            }
            Check();
            // Same-volume rename is atomic and refuses replacement. No partially
            // written engine is ever exposed to the main model picker.
            fs::rename(temp,destination);published=true;
            {std::lock_guard<std::mutex> lock(mx);state.engine=destination;state.archivedOnnx=archived;}
        } catch(...) {
            std::error_code e;
            if(!published&&contractCreated)fs::remove(destinationContract,e);
            if(!published&&archiveCreated)fs::remove(archived,e);
            throw;
        }
        // Preserve the original even after success. ONNX may refer to external
        // tensors or be in use elsewhere; the separate archive is a safe copy.
        Finish(true);
    }
};
} // converter
} // albion
