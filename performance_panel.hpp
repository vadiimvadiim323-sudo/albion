#pragma once
#include <windows.h>
#include <dwmapi.h>
#pragma comment(lib,"dwmapi.lib")
#include <functional>
#include <string>
#include <vector>
#include <cstdio>
#include "performance_stats.hpp"

namespace albion {
class PerformancePanel {
    HDC buffer_=nullptr;HBITMAP bitmap_=nullptr;HGDIOBJ oldBitmap_=nullptr;int bufferW_=0,bufferH_=0;
    std::wstring lastButtonText_;
    HWND window_=nullptr,button_=nullptr,diagnostics_=nullptr;
    HFONT font_=nullptr,title_=nullptr,value_=nullptr;
    std::function<void()> diagnosticAction_;
    performance::Snapshot snapshot_{};
    std::array<std::deque<double>,3> history_{};
    double lastHistory_=-1;bool benchmarkView_=false;
    static constexpr COLORREF bg=RGB(17,18,23),surface=RGB(28,29,35),text=RGB(245,245,248),muted=RGB(175,175,187),blue=RGB(255,53,72),green=RGB(223,223,232);
    static std::wstring Number(double v,bool rate=false) {if(!std::isfinite(v)||v<0)return L"N/A";wchar_t b[64]{};swprintf_s(b,rate?L"%.1f":L"%.2f",v);return b;}
    static void RectFill(HDC dc,RECT r,COLORREF c) {auto b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);}
    static void Rounded(HDC dc,RECT r,COLORREF c) {auto b=CreateSolidBrush(c);auto old=SelectObject(dc,b);auto p=SelectObject(dc,GetStockObject(NULL_PEN));RoundRect(dc,r.left,r.top,r.right,r.bottom,18,18);SelectObject(dc,p);SelectObject(dc,old);DeleteObject(b);}
    void Label(HDC dc,const std::wstring& s,RECT r,COLORREF c,HFONT font,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE) const {auto old=SelectObject(dc,font);SetTextColor(dc,c);SetBkMode(dc,TRANSPARENT);DrawTextW(dc,s.c_str(),-1,&r,flags|DT_NOPREFIX|DT_END_ELLIPSIS);SelectObject(dc,old);}
    void Layout() {
        RECT r{};GetClientRect(window_,&r);const int pad=24;
        MoveWindow(button_,pad,r.bottom-116,(std::max)(1,int(r.right)-2*pad),48,TRUE);
        MoveWindow(diagnostics_,r.right-160,12,136,32,TRUE);
    }
    void Tick() {
        snapshot_=performance::telemetry.Read();
        if(snapshot_.finished)performance::telemetry.ExportCompleted();
        if(snapshot_.uptime-lastHistory_>=.45) {
            lastHistory_=snapshot_.uptime;
            const double values[]={snapshot_.metrics[performance::InferenceFps].current,snapshot_.metrics[performance::PipelineMs].current,snapshot_.metrics[performance::Objects].current};
            for(size_t i=0;i<3;++i){history_[i].push_back(values[i]);if(history_[i].size()>90)history_[i].pop_front();}
        }
        std::wstring label=L"Run 60 s Benchmark";
        if(snapshot_.benchmarking)label=L"Recording... "+Number(snapshot_.benchmarkSeconds,true)+L" / 60 s";
        else if(snapshot_.finished)label=benchmarkView_?L"Benchmark complete — click for live view":L"Run another 60 s Benchmark";
        if(label!=lastButtonText_){SetWindowTextW(button_,label.c_str());lastButtonText_=label;}
        if(!IsIconic(window_))InvalidateRect(window_,nullptr,FALSE);
    }
    void Plot(HDC dc,RECT r,const std::deque<double>& values) const {
        if(values.size()<2)return;
        double low=std::numeric_limits<double>::infinity(),high=0;
        for(double v:values)if(v>=0){low=(std::min)(low,v);high=(std::max)(high,v);}
        if(!std::isfinite(low))return;
        const double range=(std::max)(.001,high-low);auto pen=CreatePen(PS_SOLID,2,blue);auto old=SelectObject(dc,pen);bool connected=false;size_t i=0;
        for(double v:values){const int x=r.left+int(double(i++)*double(r.right-r.left)/double(values.size()-1));if(v<0){connected=false;continue;}const int y=r.bottom-4-int((v-low)/range*double(r.bottom-r.top-8));if(connected)LineTo(dc,x,y);else MoveToEx(dc,x,y,nullptr);connected=true;}
        SelectObject(dc,old);DeleteObject(pen);
    }
    void Paint(HDC target) {
        RECT r{};GetClientRect(window_,&r);const int w=r.right,h=r.bottom;
        if(w<=0||h<=0)return;
        if(!buffer_||bufferW_!=w||bufferH_!=h) {
            ReleaseBuffer();buffer_=CreateCompatibleDC(target);bitmap_=CreateCompatibleBitmap(target,w,h);
            if(!buffer_||!bitmap_){ReleaseBuffer();return;}
            oldBitmap_=SelectObject(buffer_,bitmap_);bufferW_=w;bufferH_=h;
        }
        HDC dc=buffer_;RectFill(dc,r,bg);RectFill(dc,{0,0,w,2},blue);
        Label(dc,L"Performance Analytics",{24,12,w-176,48},text,title_);
        const bool frozen=benchmarkView_&&snapshot_.finished;
        const auto& metrics=frozen?snapshot_.benchmark:snapshot_.metrics;
        const wchar_t* titles[]={L"Inference throughput",L"Capture to result",L"Detected objects"};
        const size_t indexes[]={performance::InferenceFps,performance::PipelineMs,performance::Objects};
        const int gap=16,cw=(w-48-2*gap)/3,cardBottom=208;
        for(int i=0;i<3;++i) {
            const int left=24+i*(cw+gap);RECT card{left,62,left+cw,cardBottom};Rounded(dc,card,surface);
            Label(dc,titles[i],{left+18,74,left+cw-18,101},muted,font_);
            const auto& v=metrics[indexes[i]];std::wstring main=Number(frozen?v.average:v.current,i!=1);if(main!=L"N/A")main+=i==0?L" fps":(i==1?L" ms":L"");
            Label(dc,main,{left+18,104,left+cw-18,138},text,value_);
            Label(dc,L"avg "+Number(v.average,i!=1)+(i==0?L" fps":i==1?L" ms":L""),{left+18,140,left+cw-18,163},green,font_);
            if(!frozen)Plot(dc,{left+18,170,left+cw-18,194},history_[i]);
            else Label(dc,L"Completed 60-second measurement",{left+18,170,left+cw-18,194},muted,font_);
        }
        const int x[]={24,int(w*.38),int(w*.54),int(w*.71),int(w*.85),w-24};
        RectFill(dc,{24,224,w-24,257},surface);
        const wchar_t* headers[]={L"Metric",L"Last / 1s",L"Window Avg",L"Min",L"Max"};
        for(int i=0;i<5;++i)Label(dc,i==2&&frozen?L"60 s Avg":i==1&&frozen?L"Last / 1s":headers[i],{x[i]+8,224,x[i+1]-6,257},muted,font_);
        const wchar_t* names[]={L"FPS (fresh capture)",L"FPS (completed inference)",L"Capture call incl. wait",L"Backend wall time",L"GPU compute time",L"Capture to result",L"Result delivery wait",L"CPU decode / NMS",L"Submission queue wait"};
        const int rowH=(std::min)(36,(h-257-134)/9);
        for(size_t i=0;i<9;++i) {
            const int top=257+int(i)*rowH;if(i%2)RectFill(dc,{24,top,w-24,top+rowH},RGB(21,22,28));
            Label(dc,names[i],{x[0]+8,top,x[1]-6,top+rowH},text,font_);
            const auto& v=metrics[i];const double values[]={v.current,v.average,v.minimum,v.maximum};
            for(int j=0;j<4;++j){std::wstring s=Number(values[j],i<2);if(s!=L"N/A")s+=i<2?L" fps":L" ms";Label(dc,s,{x[j+1]+8,top,x[j+2]-6,top+rowH},j==1?green:muted,font_);}
        }
        std::wstring footer=frozen?L"Benchmark frozen | ":L"Live | ";
        footer+=L"Window "+Number(frozen?60:snapshot_.window,true)+L" s | FPS bins: 1 s | GPU N/A = timing disabled";
        Label(dc,footer,{24,h-61,w-24,h-36},muted,font_);
        Label(dc,L"Backend wall != GPU time. Fresh desktop frames != game FPS. 60 s results export to CSV.",{24,h-36,w-24,h-10},muted,font_);
        BitBlt(target,0,0,w,h,dc,0,0,SRCCOPY);
    }
    static LRESULT CALLBACK Proc(HWND h,UINT msg,WPARAM w,LPARAM l) noexcept {
        auto* p=reinterpret_cast<PerformancePanel*>(GetWindowLongPtrW(h,GWLP_USERDATA));
        if(msg==WM_NCCREATE){p=static_cast<PerformancePanel*>(reinterpret_cast<CREATESTRUCTW*>(l)->lpCreateParams);p->window_=h;SetWindowLongPtrW(h,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(p));}
        if(!p)return DefWindowProcW(h,msg,w,l);
        try {
            switch(msg) {
            case WM_SIZE: if(p->button_)p->Layout();return 0;
            case WM_GETMINMAXINFO: {auto* m=reinterpret_cast<MINMAXINFO*>(l);m->ptMinTrackSize={1040,720};return 0;}
            case WM_ERASEBKGND:return 1;
            case WM_PAINT:{PAINTSTRUCT ps{};HDC dc=BeginPaint(h,&ps);p->Paint(dc);EndPaint(h,&ps);return 0;}
            case WM_SHOWWINDOW: if(w)SetTimer(h,1,500,nullptr);else KillTimer(h,1);break;
            case WM_TIMER: if(IsWindowVisible(h)&&!IsIconic(h))p->Tick();return 0;
            case WM_DRAWITEM: {
                auto* d=reinterpret_cast<DRAWITEMSTRUCT*>(l);if(!d)break;
                const bool primary=d->CtlID==2001,pressed=(d->itemState&ODS_SELECTED)!=0;
                RectFill(d->hDC,d->rcItem,bg);
                Rounded(d->hDC,d->rcItem,primary?(pressed?RGB(135,25,40):RGB(189,38,38)):surface);
                wchar_t caption[160]{};GetWindowTextW(d->hwndItem,caption,160);p->Label(d->hDC,caption,d->rcItem,text,p->font_,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
                if(d->itemState&ODS_FOCUS){RECT focus=d->rcItem;InflateRect(&focus,-4,-4);DrawFocusRect(d->hDC,&focus);}return TRUE;
            }
            case WM_COMMAND:
                if(LOWORD(w)==2001){if(p->snapshot_.benchmarking)return 0;if(p->benchmarkView_&&p->snapshot_.finished)p->benchmarkView_=false;else {performance::telemetry.StartBenchmark();p->benchmarkView_=true;}p->Tick();return 0;}
                if(LOWORD(w)==2002&&p->diagnosticAction_){p->diagnosticAction_();return 0;}
                if(LOWORD(w)==IDCANCEL){ShowWindow(h,SW_HIDE);return 0;}break;
            case WM_CLOSE:ShowWindow(h,SW_HIDE);return 0;
            case WM_NCDESTROY:KillTimer(h,1);p->window_=p->button_=p->diagnostics_=nullptr;SetWindowLongPtrW(h,GWLP_USERDATA,0);break;
            }
        } catch(...) { /* Keep optional telemetry UI from terminating the pipeline. */ }
        return DefWindowProcW(h,msg,w,l);
    }
public:
    ~PerformancePanel(){Close();ReleaseBuffer();if(font_)DeleteObject(font_);if(title_)DeleteObject(title_);if(value_)DeleteObject(value_);}
    void ReleaseBuffer(){if(buffer_&&oldBitmap_)SelectObject(buffer_,oldBitmap_);if(bitmap_)DeleteObject(bitmap_);if(buffer_)DeleteDC(buffer_);buffer_=nullptr;bitmap_=nullptr;oldBitmap_=nullptr;bufferW_=bufferH_=0;}
    bool Visible() const {return window_&&IsWindowVisible(window_)&&!IsIconic(window_);}
    void Close(){if(window_)DestroyWindow(window_);window_=nullptr;diagnosticAction_={};}
    bool HandleMessage(MSG& msg){return window_&&(msg.hwnd==window_||IsChild(window_,msg.hwnd))&&IsDialogMessageW(window_,&msg);}
    void Show(HWND owner,std::function<void()> action={}) {
        diagnosticAction_=std::move(action);
        if(window_){ShowWindow(window_,SW_RESTORE);SetForegroundWindow(window_);Tick();return;}
        HINSTANCE instance=GetModuleHandleW(nullptr);WNDCLASSEXW c{sizeof(c)};c.hInstance=instance;c.lpfnWndProc=Proc;c.lpszClassName=L"AlbionPerformanceAnalytics";c.hCursor=LoadCursor(nullptr,IDC_ARROW);
        if(!RegisterClassExW(&c)&&GetLastError()!=ERROR_CLASS_ALREADY_EXISTS)return;
        font_=CreateFontW(-17,0,0,0,400,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Montserrat");
        title_=CreateFontW(-24,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Montserrat");
        value_=CreateFontW(-27,0,0,0,600,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Montserrat");
        window_=CreateWindowExW(WS_EX_TOOLWINDOW|WS_EX_CONTROLPARENT,c.lpszClassName,L"Performance Analytics — F8",WS_OVERLAPPEDWINDOW,CW_USEDEFAULT,CW_USEDEFAULT,1120,760,owner,nullptr,instance,this);
        if(!window_)return;
        { const BOOL dark=TRUE;DwmSetWindowAttribute(window_,20,&dark,sizeof(dark));
          const COLORREF border=blue,caption=bg;DwmSetWindowAttribute(window_,34,&border,sizeof(border));DwmSetWindowAttribute(window_,35,&caption,sizeof(caption));
          RECT work{},bounds{};SystemParametersInfoW(SPI_GETWORKAREA,0,&work,0);GetWindowRect(window_,&bounds);
          const int width=int(bounds.right-bounds.left),height=int(bounds.bottom-bounds.top);
          SetWindowPos(window_,nullptr,work.left+(std::max)(0,(int(work.right-work.left)-width)/2),work.top+(std::max)(0,(int(work.bottom-work.top)-height)/2),0,0,SWP_NOSIZE|SWP_NOZORDER); }
        button_=CreateWindowW(L"BUTTON",L"Run 60 s Benchmark",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,window_,reinterpret_cast<HMENU>(2001),instance,nullptr);
        diagnostics_=CreateWindowW(L"BUTTON",L"Diagnostics",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,window_,reinterpret_cast<HMENU>(2002),instance,nullptr);
        for(HWND b:{button_,diagnostics_})SendMessageW(b,WM_SETFONT,reinterpret_cast<WPARAM>(font_),TRUE);
        Layout();SetTimer(window_,1,500,nullptr);Tick();ShowWindow(window_,SW_SHOW);SetForegroundWindow(window_);
    }
};
}
