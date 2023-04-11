#include "pch.h"
#include "Utils.h"
#include <io.h>
#include <winternl.h>
#include "StrUtils.h"
#include "Logger.h"
#include <zstd.h>
#include <magnification.h>

#pragma comment(lib, "Magnification.lib")


UINT Utils::GetWindowShowCmd(HWND hwnd) {
	assert(hwnd != NULL);

	WINDOWPLACEMENT wp{};
	wp.length = sizeof(wp);
	if (!GetWindowPlacement(hwnd, &wp)) {
		Logger::Get().Win32Error("GetWindowPlacement 出错");
		assert(false);
	}

	return wp.showCmd;
}

bool Utils::GetClientScreenRect(HWND hWnd, RECT& rect) {
	if (!GetClientRect(hWnd, &rect)) {
		Logger::Get().Win32Error("GetClientRect 出错");
		return false;
	}

	POINT p{};
	if (!ClientToScreen(hWnd, &p)) {
		Logger::Get().Win32Error("ClientToScreen 出错");
		return false;
	}

	rect.bottom += p.y;
	rect.left += p.x;
	rect.right += p.x;
	rect.top += p.y;

	return true;
}

bool Utils::GetWindowFrameRect(HWND hWnd, RECT& result) {
	HRESULT hr = DwmGetWindowAttribute(hWnd,
		DWMWA_EXTENDED_FRAME_BOUNDS, &result, sizeof(result));
	if (FAILED(hr)) {
		Logger::Get().ComError("DwmGetWindowAttribute 失败", hr);
		return false;
	}

	return true;
}

bool Utils::ReadFile(const wchar_t* fileName, std::vector<BYTE>& result) {
	Logger::Get().Info(StrUtils::Concat("读取文件：", StrUtils::UTF16ToUTF8(fileName)));

	//CREATEFILE2_EXTENDED_PARAMETERS extendedParams = {};
	//extendedParams.dwSize = sizeof(CREATEFILE2_EXTENDED_PARAMETERS);
	//extendedParams.dwFileAttributes = FILE_ATTRIBUTE_NORMAL;
	//extendedParams.dwFileFlags = FILE_FLAG_SEQUENTIAL_SCAN;
	//extendedParams.dwSecurityQosFlags = SECURITY_ANONYMOUS;
	//extendedParams.lpSecurityAttributes = nullptr;
	//extendedParams.hTemplateFile = nullptr;

	//ScopedHandle hFile(SafeHandle(CreateFile2(fileName, GENERIC_READ, FILE_SHARE_READ, OPEN_EXISTING, &extendedParams)));
	 
	ScopedHandle hFile(SafeHandle(CreateFileW(fileName, GENERIC_READ, FILE_SHARE_READ,nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL| FILE_FLAG_SEQUENTIAL_SCAN, nullptr)));

	if (!hFile) {
		Logger::Get().Error("打开文件失败");
		return false;
	}
	
	DWORD size = GetFileSize(hFile.get(), nullptr);
	result.resize(size);

	DWORD readed;
	if (!::ReadFile(hFile.get(), result.data(), size, &readed, nullptr)) {
		Logger::Get().Error("读取文件失败");
		return false;
	}

	return true;
}

bool Utils::ReadTextFile(const wchar_t* fileName, std::string& result) {
	FILE* hFile;
	if (_wfopen_s(&hFile, fileName, L"rt") || !hFile) {
		Logger::Get().Error(StrUtils::Concat("打开文件 ", StrUtils::UTF16ToUTF8(fileName), " 失败"));
		return false;
	}

	// 获取文件长度
	int fd = _fileno(hFile);
	long size = _filelength(fd);

	result.clear();
	result.resize(static_cast<size_t>(size) + 1, 0);

	size_t readed = fread(result.data(), 1, size, hFile);
	result.resize(readed);

	fclose(hFile);
	return true;
}

bool Utils::WriteFile(const wchar_t* fileName, const void* buffer, size_t bufferSize) {
	FILE* hFile;
	if (_wfopen_s(&hFile, fileName, L"wb") || !hFile) {
		Logger::Get().Error(StrUtils::Concat("打开文件 ", StrUtils::UTF16ToUTF8(fileName), " 失败"));
		return false;
	}

	size_t writed = fwrite(buffer, 1, bufferSize, hFile);
	assert(writed == bufferSize);

	fclose(hFile);
	return true;
}

RTL_OSVERSIONINFOW _GetOSVersion() noexcept {
	HMODULE hNtDll = GetModuleHandle(L"ntdll.dll");
	if (!hNtDll) {
		Logger::Get().Win32Error("获取 ntdll.dll 句柄失败");
		return {};
	}
	
	auto rtlGetVersion = (LONG(WINAPI*)(PRTL_OSVERSIONINFOW))GetProcAddress(hNtDll, "RtlGetVersion");
	if (rtlGetVersion == nullptr) {
		Logger::Get().Win32Error("获取 RtlGetVersion 地址失败");
		assert(false);
		return {};
	}

	RTL_OSVERSIONINFOW version{};
	version.dwOSVersionInfoSize = sizeof(version);
	rtlGetVersion(&version);

	return version;
}

const RTL_OSVERSIONINFOW& Utils::GetOSVersion() noexcept {
	static RTL_OSVERSIONINFOW version = _GetOSVersion();
	return version;
}

struct TPContext {
	std::function<void(UINT)> func;
	std::atomic<UINT> id;
};

static void CALLBACK TPCallback(PTP_CALLBACK_INSTANCE, PVOID context, PTP_WORK) {
	TPContext* ctxt = (TPContext*)context;
	UINT id = ++ctxt->id;
	ctxt->func(id);
}

void Utils::RunParallel(std::function<void(UINT)> func, UINT times) {
#ifdef _DEBUG
	// 为了便于调试，DEBUG 模式下不使用线程池
	for (UINT i = 0; i < times; ++i) {
		func(i);
	}
#else
	if (times == 0) {
		return;
	}

	if (times == 1) {
		return func(0);
	}

	TPContext ctxt = { func, 0 };
	PTP_WORK work = CreateThreadpoolWork(TPCallback, &ctxt, nullptr);
	if (work) {
		// 在线程池中执行 times - 1 次
		for (UINT i = 1; i < times; ++i) {
			SubmitThreadpoolWork(work);
		}

		func(0);

		WaitForThreadpoolWorkCallbacks(work, FALSE);
		CloseThreadpoolWork(work);
	} else {
		Logger::Get().Win32Error("CreateThreadpoolWork 失败，回退到单线程");

		// 回退到单线程
		for (UINT i = 0; i < times; ++i) {
			func(i);
		}
	}
#endif // _DEBUG
}

bool Utils::ZstdCompress(std::span<const BYTE> src, std::vector<BYTE>& dest, int compressionLevel) {
	dest.resize(ZSTD_compressBound(src.size()));
	size_t size = ZSTD_compress(dest.data(), dest.size(), src.data(), src.size(), compressionLevel);

	if (ZSTD_isError(size)) {
		Logger::Get().Error(StrUtils::Concat("压缩失败：", ZSTD_getErrorName(size)));
		return false;
	}

	dest.resize(size);
	return true;
}

bool Utils::ZstdDecompress(std::span<const BYTE> src, std::vector<BYTE>& dest) {
	auto size = ZSTD_getFrameContentSize(src.data(), src.size());
	if (size == ZSTD_CONTENTSIZE_UNKNOWN || size == ZSTD_CONTENTSIZE_ERROR) {
		Logger::Get().Error("ZSTD_getFrameContentSize 失败");
		return false;
	}

	dest.resize(size);
	size = ZSTD_decompress(dest.data(), dest.size(), src.data(), src.size());
	if (ZSTD_isError(size)) {
		Logger::Get().Error(StrUtils::Concat("解压失败：", ZSTD_getErrorName(size)));
		return false;
	}

	dest.resize(size);

	return true;
}

bool Utils::IsStartMenu(HWND hwnd) {
	// 作为优化，首先检查窗口类
	wchar_t className[256]{};
	if (!GetClassName(hwnd, (LPWSTR)className, 256)) {
		Logger::Get().Win32Error("GetClassName 失败");
		return false;
	}

	if (std::wcscmp(className, L"Windows.UI.Core.CoreWindow")) {
		return false;
	}

	// 检查可执行文件名称
	DWORD dwProcId = 0;
	if (!GetWindowThreadProcessId(hwnd, &dwProcId)) {
		Logger::Get().Win32Error("GetWindowThreadProcessId 失败");
		return false;
	}

	Utils::ScopedHandle hProc(Utils::SafeHandle(OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, dwProcId)));
	if (!hProc) {
		Logger::Get().Win32Error("OpenProcess 失败");
		return false;
	}

	wchar_t fileName[MAX_PATH] = { 0 };
	if (!GetModuleFileNameEx(hProc.get(), NULL, fileName, MAX_PATH)) {
		Logger::Get().Win32Error("GetModuleFileName 失败");
		return false;
	}

	std::string exeName = StrUtils::UTF16ToUTF8(fileName);
	exeName = exeName.substr(exeName.find_last_of(L'\\') + 1);
	StrUtils::ToLowerCase(exeName);

	// win10: searchapp.exe 和 startmenuexperiencehost.exe
	// win11: searchhost.exe 和 startmenuexperiencehost.exe
	return exeName == "searchapp.exe" || exeName == "searchhost.exe" || exeName == "startmenuexperiencehost.exe";
}

bool Utils::SetForegroundWindow(HWND hWnd) {
	if (::SetForegroundWindow(hWnd)) {
		return true;
	}

	// 有多种原因会导致 SetForegroundWindow 失败，因此使用一个 trick 强制切换前台窗口
	// 来自 https://pinvoke.net/default.aspx/user32.SetForegroundWindow
	DWORD foreThreadId = GetWindowThreadProcessId(GetForegroundWindow(), nullptr);
	DWORD curThreadId = GetCurrentThreadId();

	if (foreThreadId != curThreadId) {
		if (!AttachThreadInput(foreThreadId, curThreadId, TRUE)) {
			Logger::Get().Win32Error("AttachThreadInput 失败");
			return false;
		}
		BringWindowToTop(hWnd);
		ShowWindow(hWnd, SW_SHOW);
		AttachThreadInput(foreThreadId, curThreadId, FALSE);
	} else {
		BringWindowToTop(hWnd);
		ShowWindow(hWnd, SW_SHOW);
	}

	return true;
}

bool Utils::ShowSystemCursor(bool value) {
	bool result = false;

	static void (WINAPI* const showSystemCursor)(BOOL bShow) = []()->void(WINAPI*)(BOOL) {
		HMODULE lib = LoadLibrary(L"user32.dll");
		if (!lib) {
			return nullptr;
		}

		return (void(WINAPI*)(BOOL))GetProcAddress(lib, "ShowSystemCursor");
	}();

	if (showSystemCursor) {
		showSystemCursor((BOOL)value);
		result = true;
	} else {
		// 获取 ShowSystemCursor 失败则回落到 Magnification API
		static bool initialized = []() {
			if (!MagInitialize()) {
				Logger::Get().Win32Error("MagInitialize 失败");
				return false;
			}
				
			return true;
		}();

		result = initialized ? (bool)ShowCursor(value) : false;
		//result = initialized ? (bool)MagShowSystemCursor(value) : false;
	}

	if (result && value) {
		// 修复有时不会立即生效的问题
		SystemParametersInfo(SPI_SETCURSORS, 0, 0, 0);
	}
	return result;
}

////////////////////////////////////////////////////////////////////////////////////////////////////////////
//
// 哈希算法来自 https://github.com/wangyi-fudan/wyhash/blob/b8b740844c2e9830fd205302df76dcdd4fadcec9/wyhash.h
//
////////////////////////////////////////////////////////////////////////////////////////////////////////////

//multiply and xor mix function, aka MUM
static uint64_t _wymix(uint64_t lhs, uint64_t rhs) noexcept {
	uint64_t hi;
	uint64_t lo = _umul128(lhs, rhs, &hi);
	return lo ^ hi;
}

//read functions
static uint64_t _wyr8(const uint8_t* p) noexcept {
	uint64_t v;
	memcpy(&v, p, 8);
	return v;
}

static uint64_t _wyr4(const uint8_t* p) noexcept {
	uint32_t v;
	memcpy(&v, p, 4);
	return v;
}

static uint64_t _wyr3(const uint8_t* p, size_t k) noexcept {
	return (((uint64_t)p[0]) << 16) | (((uint64_t)p[k >> 1]) << 8) | p[k - 1];
}

//the default secret parameters
static const uint64_t _wyp[4] = { 0xa0761d6478bd642full, 0xe7037ed1a0b428dbull, 0x8ebc6af09c88c6e3ull, 0x589965cc75374cc3ull };

uint64_t Utils::HashData(std::span<const BYTE> data) noexcept {
	const size_t len = data.size();
	uint64_t seed = _wyp[0];

	const uint8_t* p = (const uint8_t*)data.data();
	uint64_t a, b;
	if (len <= 16) {
		if (len >= 4) {
			a = (_wyr4(p) << 32) | _wyr4(p + ((len >> 3) << 2));
			b = (_wyr4(p + len - 4) << 32) | _wyr4(p + len - 4 - ((len >> 3) << 2));
		} else if (len > 0) {
			a = _wyr3(p, len);
			b = 0;
		} else {
			a = b = 0;
		}
	} else {
		size_t i = len;
		if (i > 48) {
			uint64_t see1 = seed, see2 = seed;
			do {
				seed = _wymix(_wyr8(p) ^ _wyp[1], _wyr8(p + 8) ^ seed);
				see1 = _wymix(_wyr8(p + 16) ^ _wyp[2], _wyr8(p + 24) ^ see1);
				see2 = _wymix(_wyr8(p + 32) ^ _wyp[3], _wyr8(p + 40) ^ see2);
				p += 48;
				i -= 48;
			} while (i > 48);
			seed ^= see1 ^ see2;
		}

		while (i > 16) {
			seed = _wymix(_wyr8(p) ^ _wyp[1], _wyr8(p + 8) ^ seed);
			i -= 16;
			p += 16;
		}
		a = _wyr8(p + i - 16);  b = _wyr8(p + i - 8);
	}

	return _wymix(_wyp[1] ^ len, _wymix(a ^ _wyp[1], b ^ seed));
}
