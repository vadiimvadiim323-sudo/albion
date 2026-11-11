// modules/models/models.cpp
#include "modules/app/app.hpp"

void App::StartModelConversion() {} // No compiler is shipped or invoked.
void App::PollModelConversion() {}
bool App::PauseForModelConversion(PipelineActivityWaiter&) { return false; }

void App::ScanModelFiles()
{
        mModelFiles.clear();
        const wchar_t* masks[] = { L"*.engine",L"*.plan",L"*.trt",L"*.onnx" };
        for (int directory=0;directory<2;++directory)
        for (const wchar_t* mask : masks)
        {
            std::wstring pattern = directory==0?ModelRelativePath(mask):ExeRelativePath(mask);
            WIN32_FIND_DATAW fd{}; HANDLE h = FindFirstFileW(pattern.c_str(), &fd);
            if (h == INVALID_HANDLE_VALUE) continue;
            do {
                if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
                    mModelFiles.push_back(directory==0?ModelRelativePath(fd.cFileName):ExeRelativePath(fd.cFileName));
            } while (FindNextFileW(h, &fd));
            FindClose(h);
        }
    }

std::wstring App::FileNameOf(const std::wstring& p)
{
        size_t pos = p.find_last_of(L"\\/"); return pos == std::wstring::npos ? p : p.substr(pos + 1);
    }

void App::FillModelCombo(const std::wstring& preferred)
{
        if (!mModelCombo) return;
        ScanModelFiles();
        SendMessageW(mModelCombo, CB_RESETCONTENT, 0, 0);
        if (mModelFiles.empty())
        {
            SendMessageW(mModelCombo, CB_ADDSTRING, 0,
                reinterpret_cast<LPARAM>(RU_NO_MODELS));  // <-- \u-строка
            SendMessageW(mModelCombo, CB_SETCURSEL, 0, 0);
            EnableWindow(mModelCombo, FALSE); return;
        }
        EnableWindow(mModelCombo, TRUE);
        for (auto& p : mModelFiles)
        {
            auto n = FileNameOf(p); SendMessageW(mModelCombo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(n.c_str()));
        }
        auto pref = preferred.empty()?PickModelPath():preferred; int idx = 0;
        for (size_t i = 0; i < mModelFiles.size(); ++i)
            if (_wcsicmp(mModelFiles[i].c_str(),pref.c_str())==0) { idx = int(i); break; }
        SendMessageW(mModelCombo, CB_SETCURSEL, WPARAM(idx), 0);
    }

std::wstring App::CurrentModelPath() const
{
        if (mModelCombo && !mModelFiles.empty())
        {
            int sel = int(SendMessageW(mModelCombo, CB_GETCURSEL, 0, 0));
            if (sel >= 0 && size_t(sel) < mModelFiles.size()) return mModelFiles[size_t(sel)];
        }
        return PickModelPath();
    }

void App::ApplyModel(bool force)
{
        // FIX-V11-04/08: UI posts a filename; Stop/join/loading run on the owner.
        if (mApplyingConfig) return;
        if (!force && !mLoadedConfigName.empty()) mConfigDirty = true;
        SettingsCommand next{mAimCfg, int(mAimMethod), int(mCaptureMethod), ++mSettingsRevision};
        const auto path = CurrentModelPath();
        { std::lock_guard<std::mutex> lock(mCommandMx);
          mPendingSettings = next; mPendingModel = ModelRequest{path, force}; }
        mDet.NotifyActivity();
    }
