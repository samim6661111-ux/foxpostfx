FOXPOSTFX - Vulkan post-process (sharpen/contrast/saturation/vignette)

0) GitHub chhara, phone e Termux diye build: Termux e `bash build_termux.sh` chalao,
   shesh e /sdcard/Download/libfoxpostfx.so pabe. Tarpor 4 number step e jao.
1) (ba) GitHub e notun repo banao, ei zip er shob file (.github/workflows/build.yml shoho)
   repo te upload koro. Folder structure thik rakhbe.
2) Actions tab > "build" run koro (ba push korlei auto cholbe).
3) Shesh hole artifact "libfoxpostfx" download koro > libfoxpostfx.so pabe.
4) MT Manager e APK khulo:
   - libfoxpostfx.so ke lib/arm64-v8a/ e dao
   - classes.dex edit koro, smali_snippet.txt moto line 2ta bosao
   - APK save + sign koro, install koro
5) foxpostfx.cfg ke phone e rakho:
   /storage/emulated/0/Android/data/<package>/files/foxpostfx.cfg
   (sharpen, contrast, saturation, vignette; enabled=0 dile filter bondho)
   Game cholar somoy file bodlale ~4 second pore auto reload hoy.
6) Crash hole: adb logcat | grep foxpostfx ba "foxpostfx" tag er log pathao.

NOTE: prothom bar e kaj na-o korte pare (untested on-device).
