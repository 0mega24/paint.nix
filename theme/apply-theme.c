/* Wine exposes these theme APIs by ordinal, as used by winecfg.
 * ApplyTheme also writes system colors and nonclient metrics to the registry. */
#include <windows.h>
#include <stdio.h>

typedef HRESULT (WINAPI *OpenThemeFile_t)(LPCWSTR, LPCWSTR, LPCWSTR, HANDLE *, DWORD);
typedef HRESULT (WINAPI *CloseThemeFile_t)(HANDLE);
typedef HRESULT (WINAPI *ApplyTheme_t)(HANDLE, char *, HWND);

int wmain(int argc, WCHAR **argv)
{
    static char unknown[] = "\0";
    HMODULE uxtheme = LoadLibraryW(L"uxtheme.dll");
    OpenThemeFile_t open_theme = (OpenThemeFile_t)GetProcAddress(uxtheme, MAKEINTRESOURCEA(2));
    CloseThemeFile_t close_theme = (CloseThemeFile_t)GetProcAddress(uxtheme, MAKEINTRESOURCEA(3));
    ApplyTheme_t apply_theme = (ApplyTheme_t)GetProcAddress(uxtheme, MAKEINTRESOURCEA(4));
    HANDLE theme;
    HRESULT hr;

    if (argc != 4 || !open_theme || !close_theme || !apply_theme)
    {
        fprintf(stderr, "usage: apply-theme <theme.msstyles> <ColorName> <SizeName>\n");
        return 2;
    }
    if (FAILED(hr = open_theme(argv[1], argv[2], argv[3], &theme, 0)))
    {
        fprintf(stderr, "OpenThemeFile failed: 0x%08lx\n", hr);
        return 1;
    }
    hr = apply_theme(theme, unknown, NULL);
    close_theme(theme);
    printf("ApplyTheme: 0x%08lx\n", hr);
    return FAILED(hr);
}
