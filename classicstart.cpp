#define WIN32_LEAN_AND_MEAN
#define NOMINMAX

#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <dwmapi.h>

#include <string>
#include <vector>
#include <algorithm>
#include <cwctype>

#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "dwmapi.lib")

static const wchar_t START_CLASS[] = L"ClassicStart.Native";

static HWND  g_start = nullptr;
static HHOOK g_keyboardHook = nullptr;
static HHOOK g_mouseHook = nullptr;

static HFONT g_font = nullptr;
static HFONT g_bold = nullptr;
static HFONT g_small = nullptr;

static UINT g_dpi = 96;

static bool g_startVisible = false;
static bool g_powerOpen = false;

static int g_hover = -1;
static int g_powerHover = -1;

static std::wstring g_searchText;

static bool g_lwinDown = false;
static bool g_rwinDown = false;
static bool g_winCombo = false;

static bool g_mouseTracking = false;

// ============================================================
// Colors
// ============================================================

static COLORREF g_bg;
static COLORREF g_panel;
static COLORREF g_hot;
static COLORREF g_border;
static COLORREF g_accent;
static COLORREF g_accentText;
static COLORREF g_text;
static COLORREF g_muted;

static int MinI(int a, int b)
{
    return a < b ? a : b;
}

static int MaxI(int a, int b)
{
    return a > b ? a : b;
}

static COLORREF MixColor(
    COLORREF a,
    COLORREF b,
    int percentB)
{
    percentB = MaxI(0, MinI(100, percentB));

    int ar = GetRValue(a);
    int ag = GetGValue(a);
    int ab = GetBValue(a);

    int br = GetRValue(b);
    int bg = GetGValue(b);
    int bb = GetBValue(b);

    return RGB(
        ar + (br - ar) * percentB / 100,
        ag + (bg - ag) * percentB / 100,
        ab + (bb - ab) * percentB / 100);
}

static void RefreshSystemColors()
{
    // Dark, slightly blue-black base.
    g_bg = RGB(14, 15, 18);

    // Main surface.
    g_panel = RGB(24, 26, 31);

    // Fine separators / borders.
    g_border = RGB(53, 57, 64);

    // Windows accent.
    g_accent = GetSysColor(COLOR_HIGHLIGHT);
    g_accentText = GetSysColor(COLOR_HIGHLIGHTTEXT);

    g_text = RGB(237, 240, 244);
    g_muted = RGB(145, 151, 161);

    // Subtle accent-tinted hover.
    g_hot = MixColor(
        g_panel,
        g_accent,
        16);
}

// ============================================================
// Scaling
// ============================================================

static int S(int value)
{
    return MulDiv(
        value,
        (int)g_dpi,
        96);
}

// ============================================================
// Strings
// ============================================================

static std::wstring Trim(
    const std::wstring& s)
{
    size_t a = 0;
    size_t b = s.size();

    while (a < b &&
           iswspace(s[a]))
        ++a;

    while (b > a &&
           iswspace(s[b - 1]))
        --b;

    return s.substr(
        a,
        b - a);
}

static std::wstring Lower(
    std::wstring s)
{
    std::transform(
        s.begin(),
        s.end(),
        s.begin(),
        [](wchar_t c)
        {
            return (wchar_t)towlower(c);
        });

    return s;
}

static bool StartsWithI(
    const std::wstring& a,
    const std::wstring& b)
{
    if (a.size() < b.size())
        return false;

    return Lower(
        a.substr(0, b.size())) ==
        Lower(b);
}

static bool EndsWithI(
    const std::wstring& a,
    const std::wstring& b)
{
    if (a.size() < b.size())
        return false;

    return Lower(
        a.substr(
            a.size() - b.size())) ==
        Lower(b);
}

// ============================================================
// Files
// ============================================================

static bool FileExists(
    const std::wstring& path)
{
    DWORD a =
        GetFileAttributesW(
            path.c_str());

    return
        a != INVALID_FILE_ATTRIBUTES &&
        !(a & FILE_ATTRIBUTE_DIRECTORY);
}

static bool DirectoryExists(
    const std::wstring& path)
{
    DWORD a =
        GetFileAttributesW(
            path.c_str());

    return
        a != INVALID_FILE_ATTRIBUTES &&
        (a & FILE_ATTRIBUTE_DIRECTORY);
}

// ============================================================
// Known folders
// ============================================================

static bool KnownFolder(
    REFKNOWNFOLDERID id,
    std::wstring& out)
{
    PWSTR path = nullptr;

    HRESULT hr =
        SHGetKnownFolderPath(
            id,
            0,
            nullptr,
            &path);

    if (FAILED(hr) || !path)
        return false;

    out = path;

    CoTaskMemFree(path);

    return true;
}

// ============================================================
// Environment
// ============================================================

static std::wstring ExpandEnv(
    const std::wstring& value)
{
    DWORD needed =
        ExpandEnvironmentStringsW(
            value.c_str(),
            nullptr,
            0);

    if (!needed)
        return value;

    std::vector<wchar_t> buffer(
        needed + 2);

    DWORD result =
        ExpandEnvironmentStringsW(
            value.c_str(),
            buffer.data(),
            (DWORD)buffer.size());

    return result
        ? std::wstring(buffer.data())
        : value;
}

// ============================================================
// Aliases
// ============================================================

static bool ResolveAlias(
    const std::wstring& input,
    std::wstring& out)
{
    std::wstring s =
        Lower(Trim(input));

    struct Alias
    {
        const wchar_t* name;
        REFKNOWNFOLDERID id;
    };

    static const Alias aliases[] =
    {
        { L"desktop",       FOLDERID_Desktop },
        { L"documents",     FOLDERID_Documents },
        { L"my documents",  FOLDERID_Documents },
        { L"docs",          FOLDERID_Documents },
        { L"downloads",     FOLDERID_Downloads },
        { L"pictures",      FOLDERID_Pictures },
        { L"photos",        FOLDERID_Pictures },
        { L"music",         FOLDERID_Music },
        { L"videos",        FOLDERID_Videos },
        { L"home",          FOLDERID_Profile },
        { L"user",          FOLDERID_Profile },
        { L"profile",       FOLDERID_Profile },
        { L"appdata",       FOLDERID_RoamingAppData },
        { L"localappdata",  FOLDERID_LocalAppData },
        { L"startup",       FOLDERID_Startup },
        { L"programs",      FOLDERID_Programs },
        { L"start menu",    FOLDERID_Programs }
    };

    for (const auto& a : aliases)
    {
        if (s == a.name &&
            KnownFolder(a.id, out))
        {
            return true;
        }
    }

    if (s == L"temp" ||
        s == L"tmp")
    {
        wchar_t b[MAX_PATH]{};

        DWORD n =
            GetTempPathW(
                MAX_PATH,
                b);

        if (n)
        {
            out.assign(b, n);

            while (!out.empty() &&
                   (out.back() == L'\\' ||
                    out.back() == L'/'))
            {
                out.pop_back();
            }

            return true;
        }
    }

    if (s == L"windows" ||
        s == L"win")
    {
        wchar_t b[MAX_PATH]{};

        UINT n =
            GetWindowsDirectoryW(
                b,
                MAX_PATH);

        if (n)
        {
            out.assign(b, n);
            return true;
        }
    }

    if (s == L"system32" ||
        s == L"system")
    {
        wchar_t b[MAX_PATH]{};

        UINT n =
            GetSystemDirectoryW(
                b,
                MAX_PATH);

        if (n)
        {
            out.assign(b, n);
            return true;
        }
    }

    return false;
}

// ============================================================
// Special Windows targets
// ============================================================

static bool ResolveSpecial(
    const std::wstring& input,
    std::wstring& out)
{
    std::wstring s =
        Lower(Trim(input));

    struct Special
    {
        const wchar_t* name;
        const wchar_t* target;
    };

    static const Special specials[] =
    {
        { L"this pc",          L"shell:MyComputerFolder" },
        { L"computer",         L"shell:MyComputerFolder" },
        { L"my computer",      L"shell:MyComputerFolder" },
        { L"recycle bin",      L"shell:RecycleBinFolder" },
        { L"recyclebin",       L"shell:RecycleBinFolder" },
        { L"network",          L"shell:NetworkPlacesFolder" },
        { L"control panel",    L"control.exe" },
        { L"control",          L"control.exe" },
        { L"settings",         L"ms-settings:" },
        { L"task manager",     L"taskmgr.exe" },
        { L"taskmgr",          L"taskmgr.exe" },
        { L"command prompt",   L"cmd.exe" },
        { L"cmd",              L"cmd.exe" },
        { L"powershell",       L"powershell.exe" },
        { L"terminal",         L"wt.exe" },
        { L"notepad",          L"notepad.exe" },
        { L"calculator",       L"calc.exe" },
        { L"calc",             L"calc.exe" },
        { L"registry",         L"regedit.exe" },
        { L"registry editor",  L"regedit.exe" },
        { L"regedit",          L"regedit.exe" },
        { L"device manager",   L"devmgmt.msc" },
        { L"devmgmt",          L"devmgmt.msc" },
        { L"services",         L"services.msc" },
        { L"event viewer",     L"eventvwr.msc" },
        { L"eventvwr",         L"eventvwr.msc" },
        { L"msconfig",         L"msconfig.exe" },
        { L"dxdiag",           L"dxdiag.exe" },
        { L"winver",           L"winver.exe" },
        { L"windows version",  L"winver.exe" }
    };

    for (const auto& x : specials)
    {
        if (s == x.name)
        {
            out = x.target;
            return true;
        }
    }

    return false;
}

// ============================================================
// URL / path normalization
// ============================================================

static bool IsUrl(
    const std::wstring& s)
{
    std::wstring x =
        Lower(Trim(s));

    return
        StartsWithI(x, L"http://") ||
        StartsWithI(x, L"https://") ||
        StartsWithI(x, L"ftp://") ||
        StartsWithI(x, L"mailto:") ||
        StartsWithI(x, L"ms-settings:");
}

static std::wstring NormalizePath(
    std::wstring path)
{
    path = Trim(path);

    if (path.size() >= 2 &&
        path.front() == L'"' &&
        path.back() == L'"')
    {
        path =
            path.substr(
                1,
                path.size() - 2);
    }

    path = ExpandEnv(path);

    std::replace(
        path.begin(),
        path.end(),
        L'/',
        L'\\');

    return path;
}

// ============================================================
// Search roots
// ============================================================

static void AddSearchRoot(
    std::vector<std::wstring>& roots,
    const std::wstring& path)
{
    if (path.empty())
        return;

    std::wstring lower =
        Lower(path);

    for (const auto& x : roots)
    {
        if (Lower(x) == lower)
            return;
    }

    roots.push_back(path);
}

static void BuildSearchRoots(
    std::vector<std::wstring>& roots)
{
    static const KNOWNFOLDERID ids[] =
    {
        FOLDERID_Documents,
        FOLDERID_Desktop,
        FOLDERID_Downloads,
        FOLDERID_Pictures,
        FOLDERID_Music,
        FOLDERID_Videos
    };

    for (auto id : ids)
    {
        std::wstring p;

        if (KnownFolder(id, p))
            AddSearchRoot(
                roots,
                p);
    }
}

// ============================================================
// Bounded recursive search
// ============================================================

static bool SearchDirectory(
    const std::wstring& root,
    const std::wstring& wanted,
    std::wstring& result,
    int depth,
    int& inspected)
{
    if (depth > 5 ||
        inspected > 10000)
    {
        return false;
    }

    std::wstring pattern = root;

    if (!pattern.empty() &&
        pattern.back() != L'\\')
    {
        pattern += L'\\';
    }

    pattern += L"*";

    WIN32_FIND_DATAW fd{};

    HANDLE h =
        FindFirstFileW(
            pattern.c_str(),
            &fd);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    std::wstring target =
        Lower(wanted);

    do
    {
        ++inspected;

        const wchar_t* name =
            fd.cFileName;

        if (!wcscmp(name, L".") ||
            !wcscmp(name, L".."))
        {
            continue;
        }

        std::wstring full = root;

        if (!full.empty() &&
            full.back() != L'\\')
        {
            full += L'\\';
        }

        full += name;

        if (!(fd.dwFileAttributes &
              FILE_ATTRIBUTE_DIRECTORY))
        {
            std::wstring filename =
                Lower(name);

            if (filename == target ||
                (filename.size() > target.size() &&
                 StartsWithI(
                     filename,
                     target) &&
                 filename[target.size()] == L'.'))
            {
                result = full;

                FindClose(h);
                return true;
            }
        }
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);

    h =
        FindFirstFileW(
            pattern.c_str(),
            &fd);

    if (h == INVALID_HANDLE_VALUE)
        return false;

    do
    {
        const wchar_t* name =
            fd.cFileName;

        if (!wcscmp(name, L".") ||
            !wcscmp(name, L".."))
        {
            continue;
        }

        if (!(fd.dwFileAttributes &
              FILE_ATTRIBUTE_DIRECTORY))
        {
            continue;
        }

        if (fd.dwFileAttributes &
            FILE_ATTRIBUTE_REPARSE_POINT)
        {
            continue;
        }

        std::wstring full = root;

        if (!full.empty() &&
            full.back() != L'\\')
        {
            full += L'\\';
        }

        full += name;

        if (Lower(name) == target)
        {
            result = full;

            FindClose(h);
            return true;
        }

        if (SearchDirectory(
                full,
                wanted,
                result,
                depth + 1,
                inspected))
        {
            FindClose(h);
            return true;
        }
    }
    while (FindNextFileW(h, &fd));

    FindClose(h);

    return false;
}

static bool SearchKnownFolders(
    const std::wstring& wanted,
    std::wstring& result)
{
    std::vector<std::wstring> roots;

    BuildSearchRoots(roots);

    int inspected = 0;

    for (const auto& root : roots)
    {
        if (SearchDirectory(
                root,
                wanted,
                result,
                0,
                inspected))
        {
            return true;
        }

        if (inspected > 10000)
            break;
    }

    return false;
}

// ============================================================
// Command parsing
// ============================================================

struct ParsedCommand
{
    std::wstring file;
    std::wstring args;
};

static bool ParseCommand(
    const std::wstring& input,
    ParsedCommand& out)
{
    std::wstring s =
        Trim(input);

    if (s.empty())
        return false;

    int argc = 0;

    LPWSTR* argv =
        CommandLineToArgvW(
            s.c_str(),
            &argc);

    if (!argv ||
        argc <= 0)
    {
        return false;
    }

    out.file = argv[0];
    out.args.clear();

    for (int i = 1; i < argc; ++i)
    {
        if (!out.args.empty())
            out.args += L' ';

        std::wstring arg =
            argv[i];

        bool quote =
            arg.find_first_of(
                L" \t\"") !=
            std::wstring::npos;

        if (quote)
        {
            out.args += L'"';
            out.args += arg;
            out.args += L'"';
        }
        else
        {
            out.args += arg;
        }
    }

    LocalFree(argv);

    return !out.file.empty();
}

// ============================================================
// PATH
// ============================================================

static bool FindExecutableOnPath(
    const std::wstring& name,
    std::wstring& result)
{
    std::wstring candidate =
        name;

    if (!EndsWithI(candidate, L".exe") &&
        !EndsWithI(candidate, L".com") &&
        !EndsWithI(candidate, L".bat") &&
        !EndsWithI(candidate, L".cmd"))
    {
        candidate += L".exe";
    }

    wchar_t buffer[32768]{};

    DWORD n =
        SearchPathW(
            nullptr,
            candidate.c_str(),
            nullptr,
            32768,
            buffer,
            nullptr);

    if (n && n < 32768)
    {
        result.assign(
            buffer,
            n);

        return true;
    }

    return false;
}

// ============================================================
// Relative paths
// ============================================================

static bool ResolveRelative(
    const std::wstring& input,
    std::wstring& result)
{
    std::vector<std::wstring> roots;

    BuildSearchRoots(roots);

    for (const auto& root : roots)
    {
        std::wstring candidate =
            root;

        if (!candidate.empty() &&
            candidate.back() != L'\\')
        {
            candidate += L'\\';
        }

        candidate += input;

        if (FileExists(candidate) ||
            DirectoryExists(candidate))
        {
            result = candidate;
            return true;
        }
    }

    return false;
}

// ============================================================
// Launch
// ============================================================

static bool LaunchShell(
    const std::wstring& file,
    const std::wstring& args = L"",
    const std::wstring& directory = L"")
{
    HINSTANCE r =
        ShellExecuteW(
            nullptr,
            L"open",
            file.c_str(),
            args.empty()
                ? nullptr
                : args.c_str(),
            directory.empty()
                ? nullptr
                : directory.c_str(),
            SW_SHOWNORMAL);

    return
        (INT_PTR)r > 32;
}

enum class LaunchResult
{
    Success,
    NotFound,
    Error
};

// ============================================================
// Smart execution
// ============================================================

static LaunchResult ExecuteSmartInput(
    const std::wstring& raw)
{
    std::wstring input =
        Trim(raw);

    if (input.empty())
        return LaunchResult::NotFound;

    std::wstring lower =
        Lower(input);

    if (lower == L"run" ||
        lower == L"run...")
    {
        return LaunchResult::NotFound;
    }

    if (IsUrl(input))
    {
        return LaunchShell(input)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring special;

    if (ResolveSpecial(
            input,
            special))
    {
        return LaunchShell(special)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring expanded =
        ExpandEnv(input);

    if (expanded != input)
    {
        std::wstring normalized =
            NormalizePath(expanded);

        if (FileExists(normalized) ||
            DirectoryExists(normalized))
        {
            return LaunchShell(normalized)
                ? LaunchResult::Success
                : LaunchResult::Error;
        }

        if (IsUrl(normalized))
        {
            return LaunchShell(normalized)
                ? LaunchResult::Success
                : LaunchResult::Error;
        }
    }

    std::wstring aliasPath;

    if (ResolveAlias(
            input,
            aliasPath))
    {
        return LaunchShell(aliasPath)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    ParsedCommand cmd;

    if (!ParseCommand(
            input,
            cmd))
    {
        return LaunchResult::NotFound;
    }

    std::wstring file =
        NormalizePath(cmd.file);

    if (FileExists(file) ||
        DirectoryExists(file))
    {
        return LaunchShell(
            file,
            cmd.args)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring relative;

    if (ResolveRelative(
            file,
            relative))
    {
        return LaunchShell(
            relative,
            cmd.args)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    std::wstring exe;

    if (FindExecutableOnPath(
            file,
            exe))
    {
        return LaunchShell(
            exe,
            cmd.args)
            ? LaunchResult::Success
            : LaunchResult::Error;
    }

    if (cmd.args.empty())
    {
        std::wstring found;

        if (SearchKnownFolders(
                file,
                found))
        {
            return LaunchShell(found)
                ? LaunchResult::Success
                : LaunchResult::Error;
        }
    }

    if (EndsWithI(file, L".msc") ||
        EndsWithI(file, L".cpl") ||
        EndsWithI(file, L".lnk") ||
        EndsWithI(file, L".url"))
    {
        if (LaunchShell(
                file,
                cmd.args))
        {
            return LaunchResult::Success;
        }
    }

    return LaunchResult::NotFound;
}

// ============================================================
// Native Windows Run
// ============================================================

static bool OpenNativeRun()
{
    if (g_start)
        ShowWindow(
            g_start,
            SW_HIDE);

    g_startVisible = false;
    g_powerOpen = false;
    g_hover = -1;
    g_powerHover = -1;

    INPUT in[4]{};

    in[0].type =
        INPUT_KEYBOARD;
    in[0].ki.wVk =
        VK_LWIN;

    in[1].type =
        INPUT_KEYBOARD;
    in[1].ki.wVk =
        'R';

    in[2].type =
        INPUT_KEYBOARD;
    in[2].ki.wVk =
        'R';
    in[2].ki.dwFlags =
        KEYEVENTF_KEYUP;

    in[3].type =
        INPUT_KEYBOARD;
    in[3].ki.wVk =
        VK_LWIN;
    in[3].ki.dwFlags =
        KEYEVENTF_KEYUP;

    return SendInput(
        4,
        in,
        sizeof(INPUT)) == 4;
}

// ============================================================
// Search Enter
// ============================================================

static void HandleSearchEnter()
{
    std::wstring command =
        Trim(g_searchText);

    if (command.empty())
        return;

    std::wstring lower =
        Lower(command);

    if (lower == L"run" ||
        lower == L"run...")
    {
        g_searchText.clear();

        OpenNativeRun();

        return;
    }

    LaunchResult result =
        ExecuteSmartInput(command);

    if (result ==
        LaunchResult::Success)
    {
        g_searchText.clear();

        ShowWindow(
            g_start,
            SW_HIDE);

        g_startVisible = false;
        g_powerOpen = false;

        return;
    }

    g_searchText.clear();

    OpenNativeRun();
}

// ============================================================
// GDI helpers
// ============================================================

static void FillRectColor(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    HBRUSH brush =
        CreateSolidBrush(color);

    if (!brush)
        return;

    FillRect(
        dc,
        &r,
        brush);

    DeleteObject(brush);
}

static void FillRoundRect(
    HDC dc,
    const RECT& r,
    int radius,
    COLORREF color)
{
    HBRUSH brush =
        CreateSolidBrush(color);

    if (!brush)
        return;

    HGDIOBJ old =
        SelectObject(
            dc,
            brush);

    RoundRect(
        dc,
        r.left,
        r.top,
        r.right,
        r.bottom,
        radius,
        radius);

    SelectObject(
        dc,
        old);

    DeleteObject(brush);
}

static void DrawRoundBorder(
    HDC dc,
    const RECT& r,
    int radius,
    COLORREF color)
{
    HPEN pen =
        CreatePen(
            PS_SOLID,
            1,
            color);

    if (!pen)
        return;

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            pen);

    HGDIOBJ oldBrush =
        SelectObject(
            dc,
            GetStockObject(
                NULL_BRUSH));

    RoundRect(
        dc,
        r.left,
        r.top,
        r.right,
        r.bottom,
        radius,
        radius);

    SelectObject(
        dc,
        oldBrush);

    SelectObject(
        dc,
        oldPen);

    DeleteObject(pen);
}

static void DrawTextSimple(
    HDC dc,
    const wchar_t* text,
    int x,
    int y,
    int width,
    int height,
    COLORREF color,
    HFONT font,
    UINT flags =
        DT_LEFT |
        DT_VCENTER |
        DT_SINGLELINE)
{
    if (!text || !font)
        return;

    SetBkMode(
        dc,
        TRANSPARENT);

    SetTextColor(
        dc,
        color);

    HGDIOBJ old =
        SelectObject(
            dc,
            font);

    RECT r =
    {
        x,
        y,
        x + width,
        y + height
    };

    DrawTextW(
        dc,
        text,
        -1,
        &r,
        flags);

    SelectObject(
        dc,
        old);
}

// ============================================================
// Fonts
// ============================================================

static void DestroyFonts()
{
    if (g_font)
    {
        DeleteObject(g_font);
        g_font = nullptr;
    }

    if (g_bold)
    {
        DeleteObject(g_bold);
        g_bold = nullptr;
    }

    if (g_small)
    {
        DeleteObject(g_small);
        g_small = nullptr;
    }
}

static void CreateFonts()
{
    DestroyFonts();

    g_font =
        CreateFontW(
            -S(13),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            L"Segoe UI");

    g_bold =
        CreateFontW(
            -S(15),
            0,
            0,
            0,
            FW_SEMIBOLD,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            L"Segoe UI");

    g_small =
        CreateFontW(
            -S(10),
            0,
            0,
            0,
            FW_NORMAL,
            FALSE,
            FALSE,
            FALSE,
            DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,
            CLIP_DEFAULT_PRECIS,
            CLEARTYPE_QUALITY,
            DEFAULT_PITCH |
                FF_DONTCARE,
            L"Segoe UI");
}

// ============================================================
// Work area
// ============================================================

static bool GetWorkArea(
    RECT& r)
{
    return SystemParametersInfoW(
        SPI_GETWORKAREA,
        0,
        &r,
        0) != FALSE;
}

static RECT GetStartRect()
{
    RECT work{};

    if (!GetWorkArea(work))
    {
        work =
        {
            0,
            0,
            1920,
            1080
        };
    }

    const int width =
        S(360);

    const int height =
        S(455);

    const int margin =
        S(8);

    RECT r =
    {
        work.left + margin,
        work.bottom - height - margin,
        work.left + margin + width,
        work.bottom - margin
    };

    if (r.right > work.right)
    {
        int d =
            r.right -
            work.right;

        r.left -= d;
        r.right -= d;
    }

    return r;
}

// ============================================================
// Icons
// ============================================================

static void DrawFallbackIcon(
    HDC dc,
    int type,
    int x,
    int y,
    int size,
    bool hot)
{
    COLORREF color =
        hot
            ? g_accentText
            : g_accent;

    HPEN pen =
        CreatePen(
            PS_SOLID,
            S(1),
            color);

    if (!pen)
        return;

    HGDIOBJ oldPen =
        SelectObject(
            dc,
            pen);

    int cx =
        x + size / 2;

    int cy =
        y + size / 2;

    if (type == 0)
    {
        Rectangle(
            dc,
            cx - S(8),
            cy - S(8),
            cx - S(2),
            cy - S(2));

        Rectangle(
            dc,
            cx + S(2),
            cy - S(8),
            cx + S(8),
            cy - S(2));

        Rectangle(
            dc,
            cx - S(8),
            cy + S(2),
            cx - S(2),
            cy + S(8));

        Rectangle(
            dc,
            cx + S(2),
            cy + S(2),
            cx + S(8),
            cy + S(8));
    }
    else if (type == 1)
    {
        Rectangle(
            dc,
            cx - S(8),
            cy - S(10),
            cx + S(8),
            cy + S(10));

        MoveToEx(
            dc,
            cx - S(5),
            cy - S(3),
            nullptr);

        LineTo(
            dc,
            cx + S(5),
            cy - S(3));

        MoveToEx(
            dc,
            cx - S(5),
            cy + S(2),
            nullptr);

        LineTo(
            dc,
            cx + S(5),
            cy + S(2));
    }
    else if (type == 2)
    {
        MoveToEx(
            dc,
            cx,
            cy - S(9),
            nullptr);

        LineTo(
            dc,
            cx,
            cy + S(4));

        MoveToEx(
            dc,
            cx - S(6),
            cy - S(1),
            nullptr);

        LineTo(
            dc,
            cx,
            cy + S(5));

        LineTo(
            dc,
            cx + S(6),
            cy - S(1));

        MoveToEx(
            dc,
            cx - S(9),
            cy + S(9),
            nullptr);

        LineTo(
            dc,
            cx + S(9),
            cy + S(9));
    }
    else
    {
        Rectangle(
            dc,
            cx - S(8),
            cy - S(8),
            cx + S(8),
            cy + S(8));
    }

    SelectObject(
        dc,
        oldPen);

    DeleteObject(pen);
}

static const int ITEM_COUNT = 7;

static HICON g_icons[ITEM_COUNT]{};

static void DestroyIcons()
{
    for (int i = 0;
         i < ITEM_COUNT;
         ++i)
    {
        if (g_icons[i])
        {
            DestroyIcon(
                g_icons[i]);

            g_icons[i] = nullptr;
        }
    }
}

static HICON GetStockIcon(
    SHSTOCKICONID id)
{
    SHSTOCKICONINFO info{};

    info.cbSize =
        sizeof(info);

    HRESULT hr =
        SHGetStockIconInfo(
            id,
            SHGSI_ICON |
                SHGSI_SMALLICON,
            &info);

    return SUCCEEDED(hr)
        ? info.hIcon
        : nullptr;
}

static HICON GetFileIcon(
    const wchar_t* path)
{
    if (!path)
        return nullptr;

    SHFILEINFOW sfi{};

    DWORD_PTR r =
        SHGetFileInfoW(
            path,
            0,
            &sfi,
            sizeof(sfi),
            SHGFI_ICON |
                SHGFI_SMALLICON);

    return r
        ? sfi.hIcon
        : nullptr;
}

static void CreateIcons()
{
    DestroyIcons();

    g_icons[0] =
        GetStockIcon(
            SIID_APPLICATION);

    g_icons[1] =
        GetStockIcon(
            SIID_FOLDER);

    g_icons[2] =
        GetStockIcon(
            SIID_FOLDER);

    g_icons[3] =
        GetStockIcon(
            SIID_IMAGEFILES);

    g_icons[4] =
        GetStockIcon(
            SIID_DESKTOPPC);

    g_icons[5] =
        GetFileIcon(
            L"control.exe");

    g_icons[6] =
        GetStockIcon(
            SIID_APPLICATION);

    if (!g_icons[6])
    {
        g_icons[6] =
            GetFileIcon(
                L"rundll32.exe");
    }
}

static void DrawRealIcon(
    HDC dc,
    HICON icon,
    int x,
    int y,
    int size)
{
    if (!icon)
        return;

    DrawIconEx(
        dc,
        x + S(3),
        y + S(3),
        icon,
        size - S(6),
        size - S(6),
        0,
        nullptr,
        DI_NORMAL);
}

// ============================================================
// Menu
// ============================================================

struct MenuItem
{
    const wchar_t* label;
    const wchar_t* command;
};

static const MenuItem g_items[] =
{
    { L"Programs",      L"" },
    { L"Documents",     L"documents" },
    { L"Downloads",     L"downloads" },
    { L"Pictures",      L"pictures" },
    { L"This PC",       L"this pc" },
    { L"Control Panel", L"control panel" },
    { L"Run...",         L"run" }
};

// ============================================================
// Geometry
// ============================================================

static int MenuTop()
{
    return S(70);
}

static int MenuRow()
{
    return S(42);
}

static int MenuGap()
{
    return S(3);
}

static RECT GetSearchRect(
    int width)
{
    return
    {
        S(10),
        S(10),
        width - S(10),
        S(58)
    };
}

static RECT GetPowerButtonRect(
    int width,
    int height)
{
    return
    {
        width - S(50),
        height - S(38),
        width - S(10),
        height - S(8)
    };
}

static RECT GetPowerMenuRect(
    int width,
    int height)
{
    RECT button =
        GetPowerButtonRect(
            width,
            height);

    return
    {
        button.left - S(58),
        button.top - S(48),
        button.right + S(2),
        button.top - S(6)
    };
}

static RECT GetPowerRestartRect(
    int width,
    int height)
{
    RECT menu =
        GetPowerMenuRect(
            width,
            height);

    return
    {
        menu.left + S(5),
        menu.top + S(5),
        menu.left + S(33),
        menu.bottom - S(5)
    };
}

static RECT GetPowerShutdownRect(
    int width,
    int height)
{
    RECT menu =
        GetPowerMenuRect(
            width,
            height);

    return
    {
        menu.left + S(38),
        menu.top + S(5),
        menu.left + S(66),
        menu.bottom - S(5)
    };
}

// ============================================================
// Hit testing
// ============================================================

static int HitStartItem(
    int y)
{
    const int top =
        MenuTop();

    const int row =
        MenuRow();

    const int gap =
        MenuGap();

    if (y < top)
        return -1;

    int index =
        (y - top) /
        (row + gap);

    if (index < 0 ||
        index >= ITEM_COUNT)
    {
        return -1;
    }

    int actual =
        top +
        index * (row + gap);

    return
        (y >= actual &&
         y < actual + row)
            ? index
            : -1;
}

static int HitPowerAction(
    int x,
    int y,
    int width,
    int height)
{
    if (!g_powerOpen)
        return -1;

    RECT restart =
        GetPowerRestartRect(
            width,
            height);

    RECT shutdown =
        GetPowerShutdownRect(
            width,
            height);

    POINT p{ x, y };

    if (PtInRect(
            &restart,
            p))
    {
        return 0;
    }

    if (PtInRect(
            &shutdown,
            p))
    {
        return 1;
    }

    return -1;
}

// ============================================================
// Power
// ============================================================

static void ExecutePowerAction(
    int action)
{
    g_powerOpen = false;
    g_powerHover = -1;

    if (g_start)
        ShowWindow(
            g_start,
            SW_HIDE);

    g_startVisible = false;
    g_hover = -1;

    if (action == 0)
    {
        // Restart.
        LaunchShell(
            L"shutdown.exe",
            L"/r /t 0");
    }
    else if (action == 1)
    {
        // Shutdown.
        LaunchShell(
            L"shutdown.exe",
            L"/s /t 0");
    }
}

static void DrawPowerIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    HPEN pen =
        CreatePen(
            PS_SOLID,
            S(2),
            color);

    if (!pen)
        return;

    HGDIOBJ old =
        SelectObject(
            dc,
            pen);

    int cx =
        (r.left + r.right) / 2;

    int cy =
        (r.top + r.bottom) / 2;

    int radius =
        S(8);

    Arc(
        dc,
        cx - radius,
        cy - radius,
        cx + radius,
        cy + radius,
        cx,
        cy - radius,
        cx - S(1),
        cy - radius);

    MoveToEx(
        dc,
        cx,
        cy - S(11),
        nullptr);

    LineTo(
        dc,
        cx,
        cy);

    SelectObject(
        dc,
        old);

    DeleteObject(pen);
}

static void DrawRestartIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    HPEN pen =
        CreatePen(
            PS_SOLID,
            S(2),
            color);

    if (!pen)
        return;

    HGDIOBJ old =
        SelectObject(
            dc,
            pen);

    int cx =
        (r.left + r.right) / 2;

    int cy =
        (r.top + r.bottom) / 2;

    int radius =
        S(7);

    Arc(
        dc,
        cx - radius,
        cy - radius,
        cx + radius,
        cy + radius,
        cx + radius,
        cy - S(3),
        cx + radius,
        cy - S(4));

    MoveToEx(
        dc,
        cx + S(8),
        cy - S(5),
        nullptr);

    LineTo(
        dc,
        cx + S(3),
        cy - S(8));

    LineTo(
        dc,
        cx + S(3),
        cy - S(2));

    SelectObject(
        dc,
        old);

    DeleteObject(pen);
}

static void DrawShutdownIcon(
    HDC dc,
    const RECT& r,
    COLORREF color)
{
    HPEN pen =
        CreatePen(
            PS_SOLID,
            S(2),
            color);

    if (!pen)
        return;

    HGDIOBJ old =
        SelectObject(
            dc,
            pen);

    int cx =
        (r.left + r.right) / 2;

    int cy =
        (r.top + r.bottom) / 2;

    int radius =
        S(7);

    Arc(
        dc,
        cx - radius,
        cy - radius,
        cx + radius,
        cy + radius,
        cx,
        cy - radius,
        cx - S(1),
        cy - radius);

    MoveToEx(
        dc,
        cx,
        cy - S(10),
        nullptr);

    LineTo(
        dc,
        cx,
        cy);

    SelectObject(
        dc,
        old);

    DeleteObject(pen);
}

// ============================================================
// Rounded window styling
// ============================================================

static void ApplyWindowRounding(
    HWND hwnd)
{
    if (!hwnd)
        return;

    // Windows 11.
    // Dynamically resolve this so the binary remains usable
    // on older Windows versions.
    HMODULE dwm =
        GetModuleHandleW(
            L"dwmapi.dll");

    if (!dwm)
        dwm =
            LoadLibraryW(
                L"dwmapi.dll");

    if (dwm)
    {
        using DwmSetWindowAttributeFn =
            HRESULT(WINAPI*)(
                HWND,
                DWORD,
                LPCVOID,
                DWORD);

        auto proc =
            reinterpret_cast<
                DwmSetWindowAttributeFn>(
                GetProcAddress(
                    dwm,
                    "DwmSetWindowAttribute"));

        if (proc)
        {
            // DWMWA_WINDOW_CORNER_PREFERENCE = 33
            constexpr DWORD
                DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL =
                    33;

            // DWMWCP_ROUND = 2
            constexpr DWORD
                DWMWCP_ROUND_LOCAL =
                    2;

            proc(
                hwnd,
                DWMWA_WINDOW_CORNER_PREFERENCE_LOCAL,
                &DWMWCP_ROUND_LOCAL,
                sizeof(DWMWCP_ROUND_LOCAL));

            // DWMWA_BORDER_COLOR = 34
            constexpr DWORD
                DWMWA_BORDER_COLOR_LOCAL =
                    34;

            // DWMWA_COLOR_NONE
            constexpr COLORREF
                DWMWA_COLOR_NONE_LOCAL =
                    0xFFFFFFFE;

            proc(
                hwnd,
                DWMWA_BORDER_COLOR_LOCAL,
                &DWMWA_COLOR_NONE_LOCAL,
                sizeof(DWMWA_COLOR_NONE_LOCAL));
        }
    }

    // Fallback clipping. This is deliberately fairly subtle;
    // DWM handles the actual Windows 11 rounded presentation.
    HRGN region =
        CreateRoundRectRgn(
            0,
            0,
            10000,
            10000,
            S(18),
            S(18));

    if (region)
    {
        SetWindowRgn(
            hwnd,
            region,
            TRUE);
    }
}

// ============================================================
// Close
// ============================================================

static void CloseStart()
{
    if (g_start)
    {
        ShowWindow(
            g_start,
            SW_HIDE);
    }

    g_startVisible = false;
    g_powerOpen = false;

    g_hover = -1;
    g_powerHover = -1;
}

// ============================================================
// Paint
// ============================================================

static void PaintStart(
    HWND hwnd,
    HDC dc)
{
    RECT client{};

    GetClientRect(
        hwnd,
        &client);

    int width =
        client.right;

    int height =
        client.bottom;

    HDC back =
        CreateCompatibleDC(dc);

    if (!back)
        return;

    HBITMAP bitmap =
        CreateCompatibleBitmap(
            dc,
            width,
            height);

    if (!bitmap)
    {
        DeleteDC(back);
        return;
    }

    HGDIOBJ old =
        SelectObject(
            back,
            bitmap);

    // --------------------------------------------------------
    // Main background
    // --------------------------------------------------------

    FillRectColor(
        back,
        client,
        g_bg);

    // Soft outer border.
    DrawRoundBorder(
        back,
        client,
        S(18),
        g_border);

    // --------------------------------------------------------
    // Search
    // --------------------------------------------------------

    RECT search =
        GetSearchRect(width);

    FillRoundRect(
        back,
        search,
        S(12),
        g_panel);

    DrawRoundBorder(
        back,
        search,
        S(12),
        g_border);

    // Accent caret / prompt.
    DrawTextSimple(
        back,
        L">",
        S(20),
        S(10),
        S(22),
        S(48),
        g_accent,
        g_bold);

    const wchar_t* display =
        g_searchText.empty()
            ? L"Type a command, app, file..."
            : g_searchText.c_str();

    DrawTextSimple(
        back,
        display,
        S(46),
        S(10),
        width - S(58),
        S(48),
        g_searchText.empty()
            ? g_muted
            : g_text,
        g_font);

    // --------------------------------------------------------
    // Menu rows
    // --------------------------------------------------------

    const int top =
        MenuTop();

    const int row =
        MenuRow();

    const int gap =
        MenuGap();

    for (int i = 0;
         i < ITEM_COUNT;
         ++i)
    {
        int y =
            top +
            i * (row + gap);

        bool selected =
            i == g_hover;

        RECT item =
        {
            S(7),
            y,
            width - S(7),
            y + row
        };

        COLORREF itemColor =
            selected
                ? g_accent
                : g_panel;

        FillRoundRect(
            back,
            item,
            S(9),
            itemColor);

        DrawRoundBorder(
            back,
            item,
            S(9),
            selected
                ? MixColor(
                    g_accent,
                    RGB(255,255,255),
                    18)
                : g_border);

        // ----------------------------------------------------
        // Icon tile
        // ----------------------------------------------------

        RECT icon =
        {
            S(14),
            y + S(7),
            S(43),
            y + S(36)
        };

        COLORREF iconBackground =
            selected
                ? MixColor(
                    g_accent,
                    RGB(255,255,255),
                    9)
                : MixColor(
                    g_panel,
                    g_text,
                    6);

        FillRoundRect(
            back,
            icon,
            S(7),
            iconBackground);

        DrawRoundBorder(
            back,
            icon,
            S(7),
            selected
                ? g_accentText
                : g_border);

        if (g_icons[i])
        {
            DrawRealIcon(
                back,
                g_icons[i],
                icon.left,
                icon.top,
                icon.right -
                    icon.left);
        }
        else
        {
            DrawFallbackIcon(
                back,
                i,
                icon.left,
                icon.top,
                icon.right -
                    icon.left,
                selected);
        }

        // ----------------------------------------------------
        // Label
        // ----------------------------------------------------

        DrawTextSimple(
            back,
            g_items[i].label,
            S(56),
            y,
            width - S(82),
            row,
            selected
                ? g_accentText
                : g_text,
            i == 6
                ? g_bold
                : g_font);

        // ----------------------------------------------------
        // Run shortcut
        // ----------------------------------------------------

        if (i == 6)
        {
            DrawTextSimple(
                back,
                L"WIN+R",
                width - S(76),
                y,
                S(64),
                row,
                selected
                    ? g_accentText
                    : g_muted,
                g_small,
                DT_RIGHT |
                    DT_VCENTER |
                    DT_SINGLELINE);
        }
    }

    // --------------------------------------------------------
    // Bottom utility strip
    //
    // No labels. Just a subtle divider and power control.
    // --------------------------------------------------------

    const int utilityTop =
        height - S(47);

    HPEN separator =
        CreatePen(
            PS_SOLID,
            1,
            MixColor(
                g_bg,
                g_text,
                7));

    if (separator)
    {
        HGDIOBJ oldPen =
            SelectObject(
                back,
                separator);

        MoveToEx(
            back,
            S(10),
            utilityTop,
            nullptr);

        LineTo(
            back,
            width - S(10),
            utilityTop);

        SelectObject(
            back,
            oldPen);

        DeleteObject(separator);
    }

    // --------------------------------------------------------
    // Power button
    // --------------------------------------------------------

    RECT power =
        GetPowerButtonRect(
            width,
            height);

    bool powerHot =
        g_powerHover == 2;

    bool powerActive =
        g_powerOpen;

    COLORREF powerColor =
        powerHot || powerActive
            ? g_accent
            : g_panel;

    FillRoundRect(
        back,
        power,
        S(9),
        powerColor);

    DrawRoundBorder(
        back,
        power,
        S(9),
        powerHot || powerActive
            ? MixColor(
                g_accent,
                RGB(255,255,255),
                18)
            : g_border);

    DrawPowerIcon(
        back,
        power,
        powerHot || powerActive
            ? g_accentText
            : g_muted);

    // --------------------------------------------------------
    // Power flyout
    // --------------------------------------------------------

    if (g_powerOpen)
    {
        RECT popup =
            GetPowerMenuRect(
                width,
                height);

        FillRoundRect(
            back,
            popup,
            S(10),
            MixColor(
                g_panel,
                g_bg,
                25));

        DrawRoundBorder(
            back,
            popup,
            S(10),
            g_border);

        RECT restart =
            GetPowerRestartRect(
                width,
                height);

        RECT shutdown =
            GetPowerShutdownRect(
                width,
                height);

        bool restartHot =
            g_powerHover == 0;

        bool shutdownHot =
            g_powerHover == 1;

        FillRoundRect(
            back,
            restart,
            S(7),
            restartHot
                ? g_accent
                : MixColor(
                    g_panel,
                    g_text,
                    5));

        FillRoundRect(
            back,
            shutdown,
            S(7),
            shutdownHot
                ? g_accent
                : MixColor(
                    g_panel,
                    g_text,
                    5));

        DrawRestartIcon(
            back,
            restart,
            restartHot
                ? g_accentText
                : g_text);

        DrawShutdownIcon(
            back,
            shutdown,
            shutdownHot
                ? g_accentText
                : g_text);
    }

    // --------------------------------------------------------
    // Present
    // --------------------------------------------------------

    BitBlt(
        dc,
        0,
        0,
        width,
        height,
        back,
        0,
        0,
        SRCCOPY);

    SelectObject(
        back,
        old);

    DeleteObject(bitmap);
    DeleteDC(back);
}

// ============================================================
// Window procedure
// ============================================================

static LRESULT CALLBACK StartProc(
    HWND hwnd,
    UINT msg,
    WPARAM wp,
    LPARAM lp)
{
    switch (msg)
    {
        case WM_MOUSEMOVE:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            int oldHover =
                g_hover;

            int oldPowerHover =
                g_powerHover;

            g_hover =
                HitStartItem(y);

            g_powerHover = -1;

            RECT power =
                GetPowerButtonRect(
                    client.right,
                    client.bottom);

            POINT point
            {
                x,
                y
            };

            if (PtInRect(
                    &power,
                    point))
            {
                g_powerHover = 2;
                g_hover = -1;
            }

            int powerAction =
                HitPowerAction(
                    x,
                    y,
                    client.right,
                    client.bottom);

            if (powerAction >= 0)
            {
                g_powerHover =
                    powerAction;
                g_hover = -1;
            }

            if (oldHover != g_hover ||
                oldPowerHover !=
                    g_powerHover)
            {
                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);
            }

            if (!g_mouseTracking)
            {
                TRACKMOUSEEVENT tme{};

                tme.cbSize =
                    sizeof(tme);

                tme.dwFlags =
                    TME_LEAVE;

                tme.hwndTrack =
                    hwnd;

                if (TrackMouseEvent(
                        &tme))
                {
                    g_mouseTracking = true;
                }
            }

            return 0;
        }

        case WM_MOUSELEAVE:
        {
            g_hover = -1;
            g_powerHover = -1;
            g_mouseTracking = false;

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_LBUTTONDOWN:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            // Power flyout action.
            int powerAction =
                HitPowerAction(
                    x,
                    y,
                    client.right,
                    client.bottom);

            if (powerAction >= 0)
            {
                ExecutePowerAction(
                    powerAction);

                return 0;
            }

            // Main power button.
            RECT power =
                GetPowerButtonRect(
                    client.right,
                    client.bottom);

            POINT point
            {
                x,
                y
            };

            if (PtInRect(
                    &power,
                    point))
            {
                g_powerOpen =
                    !g_powerOpen;

                g_powerHover = -1;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                return 0;
            }

            // Clicking anywhere else closes the power flyout.
            if (g_powerOpen)
            {
                RECT flyout =
                    GetPowerMenuRect(
                        client.right,
                        client.bottom);

                if (!PtInRect(
                        &flyout,
                        point))
                {
                    g_powerOpen = false;
                    g_powerHover = -1;

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);
                }
            }

            int item =
                HitStartItem(y);

            if (item == 6)
            {
                OpenNativeRun();
                return 0;
            }

            if (item >= 0 &&
                item < ITEM_COUNT)
            {
                if (item == 0)
                {
                    ShellExecuteW(
                        nullptr,
                        L"open",
                        L"shell:AppsFolder",
                        nullptr,
                        nullptr,
                        SW_SHOWNORMAL);

                    CloseStart();

                    return 0;
                }

                std::wstring command =
                    g_items[item].command;

                if (command == L"run")
                {
                    OpenNativeRun();
                    return 0;
                }

                ExecuteSmartInput(
                    command);

                CloseStart();

                return 0;
            }

            return 0;
        }

        case WM_RBUTTONDOWN:
        {
            int x =
                (int)(short)LOWORD(lp);

            int y =
                (int)(short)HIWORD(lp);

            RECT client{};

            GetClientRect(
                hwnd,
                &client);

            RECT power =
                GetPowerButtonRect(
                    client.right,
                    client.bottom);

            POINT point
            {
                x,
                y
            };

            if (PtInRect(
                    &power,
                    point))
            {
                // Right click power = restart immediately.
                ExecutePowerAction(0);
                return 0;
            }

            if (g_powerOpen)
            {
                g_powerOpen = false;
                g_powerHover = -1;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                return 0;
            }

            return 0;
        }

        case WM_KEYDOWN:
        {
            if (wp == VK_ESCAPE)
            {
                CloseStart();
                return 0;
            }

            if (wp == VK_RETURN)
            {
                HandleSearchEnter();
                return 0;
            }

            if (wp == VK_BACK)
            {
                if (!g_searchText.empty())
                {
                    g_searchText.pop_back();

                    InvalidateRect(
                        hwnd,
                        nullptr,
                        FALSE);
                }

                return 0;
            }

            break;
        }

        case WM_CHAR:
        {
            wchar_t c =
                (wchar_t)wp;

            if (c >= 32 &&
                c != 127)
            {
                g_searchText += c;

                InvalidateRect(
                    hwnd,
                    nullptr,
                    FALSE);

                return 0;
            }

            break;
        }

        case WM_ERASEBKGND:
            return 1;

        case WM_THEMECHANGED:
        case WM_SETTINGCHANGE:
        {
            RefreshSystemColors();

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_DPICHANGED:
        {
            g_dpi =
                LOWORD(wp);

            if (!g_dpi)
                g_dpi = 96;

            CreateFonts();

            RECT* suggested =
                reinterpret_cast<
                    RECT*>(lp);

            if (suggested)
            {
                SetWindowPos(
                    hwnd,
                    nullptr,
                    suggested->left,
                    suggested->top,
                    suggested->right -
                        suggested->left,
                    suggested->bottom -
                        suggested->top,
                    SWP_NOZORDER |
                        SWP_NOACTIVATE);
            }

            ApplyWindowRounding(
                hwnd);

            InvalidateRect(
                hwnd,
                nullptr,
                FALSE);

            return 0;
        }

        case WM_NCHITTEST:
        {
            // Keep this a normal interactive popup.
            return HTCLIENT;
        }

        case WM_PAINT:
        {
            PAINTSTRUCT ps{};

            HDC dc =
                BeginPaint(
                    hwnd,
                    &ps);

            PaintStart(
                hwnd,
                dc);

            EndPaint(
                hwnd,
                &ps);

            return 0;
        }

        case WM_DESTROY:
        {
            g_startVisible = false;
            g_powerOpen = false;
            return 0;
        }
    }

    return DefWindowProcW(
        hwnd,
        msg,
        wp,
        lp);
}

// ============================================================
// Create Start
// ============================================================

static bool CreateStartWindow(
    HINSTANCE instance)
{
    WNDCLASSEXW wc{};

    wc.cbSize =
        sizeof(wc);

    wc.style =
        CS_HREDRAW |
        CS_VREDRAW;

    wc.lpfnWndProc =
        StartProc;

    wc.hInstance =
        instance;

    wc.hCursor =
        LoadCursorW(
            nullptr,
            IDC_ARROW);

    wc.lpszClassName =
        START_CLASS;

    wc.hbrBackground =
        nullptr;

    if (!RegisterClassExW(
            &wc))
    {
        if (GetLastError() !=
            ERROR_CLASS_ALREADY_EXISTS)
        {
            return false;
        }
    }

    RECT r =
        GetStartRect();

    g_start =
        CreateWindowExW(
            WS_EX_TOOLWINDOW |
                WS_EX_TOPMOST |
                WS_EX_LAYERED,
            START_CLASS,
            L"ClassicStart",
            WS_POPUP,
            r.left,
            r.top,
            r.right - r.left,
            r.bottom - r.top,
            nullptr,
            nullptr,
            instance,
            nullptr);

    if (!g_start)
        return false;

    // Slight translucency.
    SetLayeredWindowAttributes(
        g_start,
        0,
        242,
        LWA_ALPHA);

    ApplyWindowRounding(
        g_start);

    return true;
}

// ============================================================
// Show / toggle
// ============================================================

static void ShowStart()
{
    if (!g_start)
        return;

    RefreshSystemColors();

    RECT r =
        GetStartRect();

    SetWindowPos(
        g_start,
        HWND_TOPMOST,
        r.left,
        r.top,
        r.right - r.left,
        r.bottom - r.top,
        SWP_SHOWWINDOW);

    g_startVisible = true;

    g_powerOpen = false;

    g_hover = -1;
    g_powerHover = -1;

    SetForegroundWindow(
        g_start);

    InvalidateRect(
        g_start,
        nullptr,
        FALSE);

    UpdateWindow(
        g_start);
}

static void ToggleStart()
{
    if (g_startVisible)
        CloseStart();
    else
        ShowStart();
}

// ============================================================
// Keyboard conversion
// ============================================================

static bool IsPrintableKey(
    DWORD vk)
{
    return
        (vk >= '0' &&
         vk <= '9') ||
        (vk >= 'A' &&
         vk <= 'Z') ||
        vk == VK_SPACE ||
        vk == VK_OEM_PERIOD ||
        vk == VK_OEM_MINUS ||
        vk == VK_OEM_PLUS ||
        vk == VK_OEM_1 ||
        vk == VK_OEM_2 ||
        vk == VK_OEM_3 ||
        vk == VK_OEM_4 ||
        vk == VK_OEM_5 ||
        vk == VK_OEM_6 ||
        vk == VK_OEM_7;
}

static wchar_t VirtualKeyToChar(
    DWORD vk)
{
    BYTE keyboard[256]{};

    if (!GetKeyboardState(
            keyboard))
    {
        return 0;
    }

    UINT scan =
        MapVirtualKeyW(
            vk,
            MAPVK_VK_TO_VSC);

    wchar_t output[4]{};

    int result =
        ToUnicode(
            vk,
            scan,
            keyboard,
            output,
            3,
            0);

    return result == 1
        ? output[0]
        : 0;
}

// ============================================================
// Keyboard hook
// ============================================================

static LRESULT CALLBACK KeyboardProc(
    int code,
    WPARAM wp,
    LPARAM lp)
{
    if (code != HC_ACTION)
    {
        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    KBDLLHOOKSTRUCT* key =
        reinterpret_cast<
            KBDLLHOOKSTRUCT*>(lp);

    if (!key)
    {
        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    if (key->flags &
        LLKHF_INJECTED)
    {
        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    DWORD vk =
        key->vkCode;

    bool down =
        wp == WM_KEYDOWN ||
        wp == WM_SYSKEYDOWN;

    bool up =
        wp == WM_KEYUP ||
        wp == WM_SYSKEYUP;

    // --------------------------------------------------------
    // Left Windows key
    // --------------------------------------------------------

    if (vk == VK_LWIN)
    {
        if (down)
        {
            g_lwinDown = true;
            g_winCombo = false;

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }

        if (up)
        {
            bool standalone =
                g_lwinDown &&
                !g_winCombo;

            g_lwinDown = false;

            if (!g_lwinDown &&
                !g_rwinDown)
            {
                g_winCombo = false;
            }

            if (standalone)
            {
                ToggleStart();
                return 1;
            }

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }
    }

    // --------------------------------------------------------
    // Right Windows key
    // --------------------------------------------------------

    if (vk == VK_RWIN)
    {
        if (down)
        {
            g_rwinDown = true;
            g_winCombo = false;

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }

        if (up)
        {
            bool standalone =
                g_rwinDown &&
                !g_winCombo;

            g_rwinDown = false;

            if (!g_lwinDown &&
                !g_rwinDown)
            {
                g_winCombo = false;
            }

            if (standalone)
            {
                ToggleStart();
                return 1;
            }

            return CallNextHookEx(
                g_keyboardHook,
                code,
                wp,
                lp);
        }
    }

    // --------------------------------------------------------
    // Preserve native Windows shortcuts.
    // --------------------------------------------------------

    if (down &&
        (g_lwinDown ||
         g_rwinDown))
    {
        g_winCombo = true;

        if (g_startVisible)
            CloseStart();

        return CallNextHookEx(
            g_keyboardHook,
            code,
            wp,
            lp);
    }

    // --------------------------------------------------------
    // Ctrl+Esc
    // --------------------------------------------------------

    if (down &&
        vk == VK_ESCAPE &&
        (GetKeyState(VK_CONTROL) &
            0x8000))
    {
        ToggleStart();
        return 1;
    }

    // --------------------------------------------------------
    // Start search
    // --------------------------------------------------------

    if (g_startVisible &&
        down)
    {
        if (vk == VK_ESCAPE)
        {
            CloseStart();
            return 1;
        }

        if (vk == VK_RETURN)
        {
            HandleSearchEnter();
            return 1;
        }

        if (vk == VK_BACK)
        {
            if (!g_searchText.empty())
            {
                g_searchText.pop_back();

                InvalidateRect(
                    g_start,
                    nullptr,
                    FALSE);
            }

            return 1;
        }

        if (IsPrintableKey(vk))
        {
            wchar_t c =
                VirtualKeyToChar(vk);

            if (c >= 32)
            {
                g_searchText += c;

                InvalidateRect(
                    g_start,
                    nullptr,
                    FALSE);

                return 1;
            }
        }
    }

    return CallNextHookEx(
        g_keyboardHook,
        code,
        wp,
        lp);
}

// ============================================================
// Taskbar Start click
// ============================================================

static bool IsStartButtonClick(
    POINT point)
{
    HWND taskbar =
        FindWindowW(
            L"Shell_TrayWnd",
            nullptr);

    if (!taskbar)
        return false;

    RECT taskbarRect{};

    if (!GetWindowRect(
            taskbar,
            &taskbarRect))
    {
        return false;
    }

    if (!PtInRect(
            &taskbarRect,
            point))
    {
        return false;
    }

    int taskbarHeight =
        taskbarRect.bottom -
        taskbarRect.top;

    int usableHeight =
        taskbarHeight > S(32)
            ? taskbarHeight
            : S(32);

    RECT left =
    {
        taskbarRect.left,
        taskbarRect.bottom -
            usableHeight,
        taskbarRect.left + S(62),
        taskbarRect.bottom
    };

    if (PtInRect(
            &left,
            point))
    {
        return true;
    }

    int center =
        taskbarRect.left +
        (taskbarRect.right -
         taskbarRect.left) / 2;

    RECT centered =
    {
        center - S(34),
        taskbarRect.bottom -
            usableHeight,
        center + S(34),
        taskbarRect.bottom
    };

    return PtInRect(
        &centered,
        point) != FALSE;
}

// ============================================================
// Mouse hook
// ============================================================

static LRESULT CALLBACK MouseProc(
    int code,
    WPARAM wp,
    LPARAM lp)
{
    if (code == HC_ACTION)
    {
        MSLLHOOKSTRUCT* mouse =
            reinterpret_cast<
                MSLLHOOKSTRUCT*>(lp);

        if (mouse &&
            !(mouse->flags &
              LLMHF_INJECTED) &&
            wp == WM_LBUTTONDOWN)
        {
            POINT point =
                mouse->pt;

            if (IsStartButtonClick(
                    point))
            {
                ToggleStart();
                return 1;
            }

            if (g_startVisible)
            {
                RECT r =
                    GetStartRect();

                if (!PtInRect(
                        &r,
                        point))
                {
                    CloseStart();
                }
            }
        }
    }

    return CallNextHookEx(
        g_mouseHook,
        code,
        wp,
        lp);
}

// ============================================================
// DPI
// ============================================================

static void InitializeDpi()
{
    HMODULE user32 =
        GetModuleHandleW(
            L"user32.dll");

    if (user32)
    {
        typedef BOOL(WINAPI*
            SetDpiContextProc)(
                DPI_AWARENESS_CONTEXT);

        auto proc =
            reinterpret_cast<
                SetDpiContextProc>(
                GetProcAddress(
                    user32,
                    "SetProcessDpiAwarenessContext"));

        if (proc)
        {
            proc(
                DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        }
    }

    HDC dc =
        GetDC(nullptr);

    if (dc)
    {
        g_dpi =
            GetDeviceCaps(
                dc,
                LOGPIXELSX);

        if (!g_dpi)
            g_dpi = 96;

        ReleaseDC(
            nullptr,
            dc);
    }
}

// ============================================================
// Entry
// ============================================================

int WINAPI wWinMain(
    HINSTANCE instance,
    HINSTANCE,
    PWSTR,
    int)
{
    HRESULT com =
        CoInitializeEx(
            nullptr,
            COINIT_APARTMENTTHREADED |
                COINIT_DISABLE_OLE1DDE);

    InitializeDpi();

    RefreshSystemColors();

    CreateFonts();

    CreateIcons();

    if (!CreateStartWindow(
            instance))
    {
        MessageBoxW(
            nullptr,
            L"Could not create ClassicStart.",
            L"ClassicStart",
            MB_OK |
                MB_ICONERROR);

        DestroyIcons();
        DestroyFonts();

        if (SUCCEEDED(com))
            CoUninitialize();

        return 1;
    }

    g_keyboardHook =
        SetWindowsHookExW(
            WH_KEYBOARD_LL,
            KeyboardProc,
            instance,
            0);

    if (!g_keyboardHook)
    {
        MessageBoxW(
            nullptr,
            L"Could not install the keyboard hook.",
            L"ClassicStart",
            MB_OK |
                MB_ICONERROR);

        DestroyWindow(
            g_start);

        g_start = nullptr;

        DestroyIcons();
        DestroyFonts();

        if (SUCCEEDED(com))
            CoUninitialize();

        return 1;
    }

    g_mouseHook =
        SetWindowsHookExW(
            WH_MOUSE_LL,
            MouseProc,
            instance,
            0);

    if (!g_mouseHook)
    {
        MessageBoxW(
            nullptr,
            L"Could not install the mouse hook.",
            L"ClassicStart",
            MB_OK |
                MB_ICONERROR);

        UnhookWindowsHookEx(
            g_keyboardHook);

        g_keyboardHook = nullptr;

        DestroyWindow(
            g_start);

        g_start = nullptr;

        DestroyIcons();
        DestroyFonts();

        if (SUCCEEDED(com))
            CoUninitialize();

        return 1;
    }

    MSG msg{};

    while (true)
    {
        BOOL result =
            GetMessageW(
                &msg,
                nullptr,
                0,
                0);

        if (result <= 0)
            break;

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_mouseHook)
    {
        UnhookWindowsHookEx(
            g_mouseHook);

        g_mouseHook = nullptr;
    }

    if (g_keyboardHook)
    {
        UnhookWindowsHookEx(
            g_keyboardHook);

        g_keyboardHook = nullptr;
    }

    if (g_start)
    {
        DestroyWindow(
            g_start);

        g_start = nullptr;
    }

    DestroyIcons();
    DestroyFonts();

    if (SUCCEEDED(com))
        CoUninitialize();

    return 0;
}