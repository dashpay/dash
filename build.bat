@echo off
SETLOCAL
set PATH=%windir%\system32;%PATH% &:: override msys if it's on the PATH
set TOP=%~dp0
set CFLAGS=/nologo /c /O2 /Zi /Fdblst.pdb /W4
cl 2>&1 | find "for ARM64" > nul:
IF ERRORLEVEL 1 (
    set arm64=no
    FOR %%F IN (%TOP%build\win64\*-x86_64.asm) DO (
        ml64 /nologo /c /Cp /Cx /Zi %%F || EXIT /B
    )
) ELSE (
    set arm64=yes
    FOR %%F IN (%TOP%build\win64\*-armv8.asm) DO (
        armasm64 -nologo %%F || EXIT /B
    )
)
SETLOCAL ENABLEDELAYEDEXPANSION
set static=/out:blst.lib
set shared=
set arm64x=
FOR %%O IN (%*) DO (
    set opt=%%O
    IF "!opt!" == "-shared" (
        IF [!shared!] EQU [] set shared=/out:blst.dll
    ) ELSE IF "!opt!" == "-dll" (
        IF [!shared!] EQU [] set shared=/out:blst.dll
    ) ELSE IF "!opt:~0,5!" == "/out:" (
	IF "!opt:~-4!" == ".dll" (set shared=!opt!) ELSE (set static=!opt!)
    ) ELSE IF "!opt!" == "-arm64x" (
        set arm64x=%arm64%
    )
)
IF [%shared%] NEQ [] (
    cl %CFLAGS% /MD /D__BLST_DLL_MAIN__ %TOP%src\server.c || EXIT /B
    set ld=
    FOR /F "usebackq delims=" %%F IN (`where link`) DO (
        IF "!ld!" == "" (
            "%%F" 2>&1 | find "Linker" > nul:
            IF !ERRORLEVEL! EQU 0 set ld="%%F"
        )
    )
    IF [%arm64x%] NEQ [yes] (
        !ld! /nologo /debug /dll /entry:DllMain /incremental:no %shared% ^
             /def:%TOP%build\win64\blst.def *.obj kernel32.lib && del *.obj
    ) ELSE (
        lib /nologo /out:blst_arm64.lib *.obj && del *.obj || EXIT /B
        FOR %%F IN (%TOP%build\win64\*-armv8.asm) DO (
            armasm64 -nologo -machine arm64ec -nowarn %%F || EXIT /B
        )
        cl /arm64EC %CFLAGS% /MD /D__BLST_DLL_MAIN__ %TOP%src\server.c || EXIT /B
        !ld! /nologo /machine:arm64x /dll /noentry %shared% ^
             /def:%TOP%build\win64\blst.def *.obj ^
             /defArm64Native:%TOP%build\win64\blst.def blst_arm64.lib ^
             kernel32.lib && del *.obj blst_arm64.lib
    )
) ELSE (
    cl %CFLAGS% /MT /Zl %TOP%src\server.c || EXIT /B
    lib /nologo %static% *.obj && del *.obj
)
ENDLOCAL
EXIT /B
