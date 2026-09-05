# AccentIME Native 构建

要求：Visual Studio 2022 Build Tools（MSVC v143）、Windows 10/11 SDK、.NET 8 SDK。

```powershell
& 'C:\BuildTools\MSBuild\Current\Bin\amd64\MSBuild.exe' .\AccentIME.Native.vcxproj /m /p:Configuration=Release /p:Platform=x64
dotnet tool install wix --tool-path .\.tools\wix --version 6.0.2
.\.tools\wix\wix.exe extension add WixToolset.UI.wixext/6.0.2
.\.tools\wix\wix.exe build .\installer\Product.wxs -arch x64 -culture zh-CN -bindpath . -ext WixToolset.UI.wixext -o .\bin\x64\Release\AccentIME.ThemeManager-x64.msi
```

应用程序使用静态 MSVC 运行库，不要求目标机器安装 VC++ Redistributable。
