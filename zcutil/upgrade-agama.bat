@echo off
call :GET_CURRENT_DIR
cd %THIS_DIR%
set /p AGAMA_DIR="Agama directory, followed by [ENTER]:"
cd C:\%UserInputPath%
echo "Removing old binaries"
rm %AGAMA_DIR%/resources/app/assets/bin/win64/verusd/*
echo "Copying files"
cp ./* %AGAMA_DIR%/resources/app/assets/bin/win64/verusd/
echo "Upgrade complete"

:GET_CURRENT_DIR
@pushd %~dp0
@set THIS_DIR=%CD%
@popd
@goto :EOF
