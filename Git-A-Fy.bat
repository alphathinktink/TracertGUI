@echo off
setlocal

echo.
echo ==================================================
echo Git-a-fy Current Folder
echo ==================================================
echo.

set "RepoName="
set /P "RepoName=Enter the new GitHub repo name: "
if "%RepoName%"=="" (
	echo.
	echo ERROR: Repo name is required.
	goto End
)

echo.
set "VisibilityChoice="
set /P "VisibilityChoice=Make repo public? [y/N]: "

set "Visibility=--private"
if /I "%VisibilityChoice%"=="Y" set "Visibility=--public"
if /I "%VisibilityChoice%"=="YES" set "Visibility=--public"

echo.
echo Repo name: %RepoName%
if "%Visibility%"=="--public" (
	echo Visibility: Public
) else (
	echo Visibility: Private
)
echo.

set "Confirm="
set /P "Confirm=Proceed? [Y/n]: "
if /I "%Confirm%"=="N" goto End
if /I "%Confirm%"=="NO" goto End

if exist ".git" (
	echo.
	echo ERROR: This folder already contains a .git repository.
	goto End
)

echo.
echo Initializing local git repository...
git init
if errorlevel 1 (
	echo.
	echo ERROR: git init failed.
	goto End
)

echo.
echo Adding files...
git add .
if errorlevel 1 (
	echo.
	echo ERROR: git add failed.
	goto End
)

echo.
echo Creating initial commit...
git commit -m "Initial commit"
if errorlevel 1 (
	echo.
	echo ERROR: git commit failed.
	echo Make sure there is at least one file to commit.
	goto End
)

echo.
echo Renaming branch to main...
git branch -M main
if errorlevel 1 (
	echo.
	echo ERROR: Failed to rename branch to main.
	goto End
)

echo.
echo Creating GitHub repository and pushing...
gh repo create "%RepoName%" %Visibility% --source=. --remote=origin --push
if errorlevel 1 (
	echo.
	echo ERROR: gh repo create failed.
	goto End
)

echo.
echo ==================================================
echo Success.
echo GitHub repo "%RepoName%" created and pushed.
echo ==================================================

:End
echo.
pause
endlocal
