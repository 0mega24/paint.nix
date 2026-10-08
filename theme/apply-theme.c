/* apply-theme.exe <theme.msstyles> <ColorName> <SizeName>
 * Applies a visual style exactly like winecfg's Desktop Integration tab:
 * uxtheme OpenThemeFile (ordinal 2) + ApplyTheme (ordinal 4), which also writes
 * the theme's system colors and nonclient metrics (fonts) into the registry. */
#include <windows.h>
#include <stdio.h>

typedef HRESULT (WINAPI *OpenThemeFile_t)(LPCWSTR, LPCWSTR, LPCWSTR, HANDLE *, DWORD);
typedef HRESULT (WINAPI *CloseThemeFile_t)(HANDLE);
typedef HRESULT (WINAPI *ApplyTheme_t)(HANDLE, char *, HWND);

int wmain(int argc, WCHAR **argv)
{
    static char unknown[] = "\0";
    HMODULE ux = LoadLibraryW(L"uxtheme.dll");
    OpenThemeFile_t open = (OpenThemeFile_t)GetProcAddress(ux, MAKEINTRESOURCEA(2));
    CloseThemeFile_t close = (CloseThemeFile_t)GetProcAddress(ux, MAKEINTRESOURCEA(3));
    ApplyTheme_t apply = (ApplyTheme_t)GetProcAddress(ux, MAKEINTRESOURCEA(4));
    HANDLE theme;
    HRESULT hr;

    if (argc != 4 || !open || !close || !apply)
    {
        fprintf(stderr, "usage: apply-theme <theme.msstyles> <ColorName> <SizeName>\n");
        return 2;
    }
    if (FAILED(hr = open(argv[1], argv[2], argv[3], &theme, 0)))
    {
        fprintf(stderr, "OpenThemeFile failed: 0x%08lx\n", hr);
        return 1;
    }
    hr = apply(theme, unknown, NULL);
    close(theme);
    printf("ApplyTheme: 0x%08lx\n", hr);
    return FAILED(hr);
}
