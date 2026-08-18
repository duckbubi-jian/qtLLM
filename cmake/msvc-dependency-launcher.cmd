@echo off
for /f "tokens=3" %%C in ('reg.exe query HKLM\SYSTEM\CurrentControlSet\Control\Nls\CodePage /v ACP 2^>nul') do chcp.com %%C >nul
%*
exit /b %errorlevel%
