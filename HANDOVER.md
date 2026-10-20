## Android installed over Wi-Fi; Press Start renders (2026-10-21)

ACTIVE TABLET: Xiaomi Pad8Pro, Android16, Adreno830. Paired through wireless ADB.
Current address192.168.1.63:41307. SDK36 adb uses isolated server port15038 with
ADB_BURST_MODE=1; normal Homebrew35 server5037 also existed. Use SDK executable
~/Library/Android/sdk/platform-tools/adb -P15038 -s192.168.1.63:41307.
Pair code was ephemeral; no code is stored in repo. Switch remains parked.

Installed FULL (nonincremental) original-icon APK and all6371 game files.
An earlier incremental install reported success but package registration was lost;
full install now stable. Installed APK byte hash verified after final update.
Original dirt2.ico is extracted at packaging and wrapped in Android adaptive icon;
artwork PNG remains generated in build/. Icon/helper checkpointd3462ec.

First startup failed before GPU: shell-created Android/data assets gave EACCES.
Fixed Android launcher to SDL_GetAndroidInternalStoragePath; both assets and
Dirt2Data now in private files/. Copied data locally via shell tar | run-as tar,
without another11GB network upload. ALL6371 private files SHA256 match PC sources,
checked under app UID. External duplicate game folder removed only after private
verification + successful startup. Sourceinstaller now stages resumable copy in
/data/local/tmp/dirt2-install-assets, extracts as app UID, removes successful stage.
No broad storage permission. Full APK updates preserve private assets/saves.

FINAL buildb0988215319e7b74fd46d20abb84c28bbfad2d36, APK SHA256
13c1e418c204def7f89ffff3347d39692d680bf3b37a1a5fcd85a5c0bf373ee6.
Package build/package/android/DiRT2-android-arm64-debug.apk,184218604bytes.
Matching ELF/nm archived under package symbols/<build-id>. Manifest installedtrue,
startup_verifiedtrue, assets_sha256_verifiedtrue; gameplay_verifiedFALSE.

Hardware result: Direct3D11 device FL11_0 created, Vulkan surface/swapchain active,
Press Start frame rendered. Screenshot build/android-install/private-startup.png
shows31.1FPS HUD; no race/steady performance claim. Physical output3200x2136;
this is NOT proof of internal render resolution. App left running on Press Start.
Input/name keyboard/race/background/Quit still need testing. No synthetic input
or career/save changes were made. Existing data source unmodified.

Evidence build/android-install/: source-sha256.json, private-sha256.txt,
private-verification.json, final-apk-verification.json, private-startup.log/png,
startup-data.tar, Vulkan capabilities. Source/full build logs in build/.
Native storage correction cross-build + signed APK/16KB validations PASS.

## Android ARM64 port built; tablet test pending (2026-10-21)

Switch parked by user; checkpoint31aadf3 (prior sampler6940c23). No further Switch
FTP changes. Android requested backend is primary DX11 -> native DXVK -> Android
system Vulkan; first target Xiaomi Pad8Pro. ADB detects NO device yet. Asked user
to connect USB debugging/authorize computer. Do not claim installed or gameplay tested.

Build: bash tools/scripts/build_android.sh. End-to-end SUCCESS in
build/android-end-to-end.log. Full540 generated sources compiled with NDK29 ARM64,
TSO ON, fast-math OFF. All native runtime/shim/SDL/JNI sources compile; static
DXVK D3D11/DXGI/D3D9 archive built from isolated prepared Android copy. Existing
runtime dispatch/cache/memory/PC regression CTests3/3PASS; native Java compiles.
Private directories Android only; Apple/Switch binary libraries untouched.

Android SDLActivity loads c++_shared, SDL3, dirt2; exported SDL_main verified.
APK minAPI28, target35, Vulkan1.3 feature requirement, landscape, gamepad input,
native Android keyboard via JNI/EditText dialog. App-scoped external assets and
separate Dirt2Data saves/settings/logs. The original dirt2.ico supplies adaptive launcher artwork; no executable or
playable game data is included in APK. No broad storage permission. Fresh+async exposure fix reused; no Switch worker remapping or sampler.
Android retains OS crash handling/tombstones instead of unavailable execinfo.
Shared image hides private static archive symbols (--exclude-libs=ALL), required
for FFmpeg NEON direct PIC references to internal FFT tables. Driver is device's
libvulkan.so, not NVK/MoltenVK. Native16KB LOAD alignment verified for all3libs;
APK signature and ZIP16KB alignment verified. Packaging enforces alignment.

APK: build/package/android/DiRT2-android-arm64-debug.apk (about176MiB).
Matching game build ID056a57131c8d53a4c5a571d42695c4c9d6d97ece.
Latest APK SHA/size in package manifest.json; ZIP/signature timestamps can change.
Unstripped game/SDL symbols archived under package symbols/<build-id>/.
Debug signing key in build/android-apk/debug.keystore; preserve for updates.
Docs docs/ANDROID.md. Install helper tools/scripts/install_android.sh accepts
optional PC game directory. When tablet connects, install/copy assets, test first
menu/race/input/keyboard/resolution/background/Quit; retrieve Dirt2Data + crash
logcat. No hardware FPS or visual correctness claims yet.

## Switch work parked by user (2026-10-20)

User considers the current DX11 version a useful playable checkpoint despite low FPS.
All Switch source changes are committed through6940c23. Latest installed build is
ba6d11682ffc46460a04c034c26cc909aab6b3fa (instruction sampler enabled); its next
hardware trace has not been collected. For ordinary play, remove DIRT2_PROFILE_PC
and DIRT2_PROFILE_PC_TID from env.txt. Do not resume Switch experiments unless asked.
DX11/DXVK/NVK remains primary; DX9/deko fallback preserved. No30FPS claim.
New requested work: Android ARM64 using native DXVK D3D11 -> device Vulkan.

## Current: Clang race trace collected; instruction sampler installed and verified

User confirms race loads with the stock worker map, but no visible FPS gain.
Disabling only DIRT2_SWITCH_PARALLEL_JOBS fixed the prior loading stall. Keep it 0.
Evidence: build/performance/switch-clang-stock-20261020-232334/analysis.json.
Late race window318–345s median10.25FPS,97.302ms frame,0.0645ms Present;
worker26492.7–93.7% core. Short window, not controlled scene comparison; no gain claim.
GPU/CS waits negligible. Full Clang CPU archive/COM cache remain enabled.

New opt-in DIRT2_PROFILE_PC samples only guest worker264 around20Hz with jitter.
Pins its pthread handle under registry mutex; pauses, reads native PC/LR, resumes
before allocation/logging. Failed reads still resume; unsupported loader SVC hints
skip sampling; repeated kernel errors disable it. Unexpected resume failure invokes
kernel panic rather than continuing with a paused worker. No stack walking.
Snapshots are wall samples, not CPU-weighted; all PC/LR bins retained per10s window.
Both sampler modes cannot combine. Normal packages do not enable instruction sampling.

Validation: native ASAN/UBSAN failure-order tests and CTest runtime_pc_sampling PASS;
full Switch cross-build PASS. tools/scripts/analyze_switch_pc.py attributes logged
relative PCs using the EXACT archived nm file. Use corresponding ELF disassembly
before optimizing a hotspot; do not assume inclusive contexts are self CPU costs.
Package build/package/switch-cpu-pc-profile, buildba6d11682ffc46460a04c034c26cc909aab6b3fa,
SHA2567e610adba371bcc50951cd79402c2c24f3be416d2fe424c774df244632da0fa6.
Matching ELF/nm archived in package symbols/. FTP installation COMPLETE: all three
files staged and fully retrieved/hash verified, then activated env/README/NRO last.
Manifest installed=true. Prior active files retained as timestamped backups.
Next hardware test: race2min, Quit, FTP; inspect prof-pc samples/errors and match symbols.
This diagnostic adds overhead;30FPS target remains unmet. DX9 fallback untouched.

## CURRENT CONSOLE: STOCK JOB MAP RESTORED; RACE LOADING RETEST NEEDED

User18a21d02 globaltestreachedmenu butraceloadedforever (statcards/spinneractive).
Retrievedbuild/performance/switch-loading-20261020-231304. Last4min~41FPS320draws,
GPU/CSwait0,compiler1299/1299complete; loadingUIalive, noGPUcompilerstall. Jobmap
collapseleadingSUSPECT,notconfirmedcause. StructuralXMLtestdidNOTprovenestedjob
progress. Do NOTre-enableexperimentalmapasdefaultorcallitvalidated.

INSTALLED/VERIFIEDconfigonlypatch: DIRT2_SWITCH_PARALLEL_JOBS=0 explicitly.
Originalshipped4Coremapusednextlaunch. Clangall540objects+COMcache1UNCHANGED.
Manifest build/package/switch-cpu-clang-stock-jobs installedtrue,config_changeonly.
NROstill18a21d0286a42d17b409b8dd9e4763ba04d0e270 SHA
4bde22ecde1d9d339fb882827eae523c1f3be4b45a496c5ec5f69197cc24e509.
NROmatchespriorverifiedglobalbuild; noNROupload. env/READMEfullRETR byteverified,
remoteoldsettingsbackedup. Exactold/newenvdiffONLYparallelflag1->0. PrivateXMLonSD
canremain; startupdoesnotregisteroverridewhenflag0. Sourcepackager nowwrites0
when--parallel-jobsomitted. Correctedzipisstock-jobspackage; oldglobalzipretains
historicalfailedparallelconfig,avoidinstallingthatoneagain.

Nextusertestraceloading. Ifloads,play2minsamecourse/settingsforClang-onlyCPUdata,
thenQuit/openFTP. Ifstillstalls,nextisolateCOMcacheandcompilerseparately;current
tracehasnoPCsamplingsoactualblockedjobunknown. FullSwitchELF/nmstillglobal/symbols.
No30FPS/improvementclaim. Quitpreviouslyconfirmedfixed. Credentialtempsremoved.

## CURRENT CONSOLE: GLOBAL CPU BUILD INSTALLED AND VERIFIED

Build18a21d0286a42d17b409b8dd9e4763ba04d0e270 isNOWACTIVE at
sdmc:/switch/dirt2-dx11/dirt2-dx11.nro. FullFTPupload/RETR SHA256verified all3files
thenactivatedenv/README/NROlast. Installer35725COMPLETE. Manifestinstalledtrue in
build/package/switch-cpu-global/manifest.json. Timestampedbackupsofprevious6fbbuild
retained. MatchingELF/nmunder symbols. No userassets/saves/settingschanged.

IncludesfullClang540objectCPUarchive + experimental5workerparallelmap +COMlookupcache.
TSOandstrictmathpreserved,D3D11/DXVK/NVK unchanged, fresh+async retained,
diskcachesOFF, lowfrequencyCPU/drivertraceON, indirectsamplerOFF. Quitalreadyuser
confirmedfixedinpriorbuild. GlobalFPS/physics/visualresultPENDING. User to test
30smenu and2minsameCroatiaracewithunchangedsettingsthenQuit/openFTP; reportFPS,
wheelmotion/physicsandanyvisualchange. DoNOTclaim30FPS/improvementyet.

Sourcecheckpointbfbd27b. Credentialinstallerremovedaftercompletion. Oldpartial
*.upload-* fromearlierfailedattemptsmayremain,butnoneisactive. Preservebackups.

## CURRENT: GLOBAL CPU PATCH BUILT; FTP INTERRUPTED BEFORE ACTIVATION

UserCONFIRMSQuitworks. Newlogsdownloadedbuild/performance/switch-20261020-223332:
314s session;145–270s racewindowmedian11.7FPS/85.566ms,Present0.0575ms,worker264~93%core.
No demonstratedgainvs11.6prior; differentmoments/resolution1280x720notcontrolledAB.
Logendscleanapplicationexitnotificationhandlerready,userconfirmedHOMEexit.

BIGGERCOMBINEDPATCH:
- FULL540generatedobjects compiledwithAppleClang21targetaarch64-none-elf against
sameSDKnewlib. TSO/PIC/A57+crypto flagsremain;fastmathOFF,fpcontractOFF,matherrnoOFF,
nooutlineatomics. RemoveGCCsoftTLSflagsONLYgeneratedC(nonoativeTLS). NativesC++ /
SDL/DXVK/NVKstillGCC SDK. Morevectorization/lesshelpercalls,code17.9%larger tradeoff.
- OptinDIRT2_SWITCH_PARALLEL_JOBS=1 installsprivateVFSoverridesforworkerMap4/8Core,
5workersderivedfrom8coremap (3simulation+2grass),solver/softbodyDISJOINT. Alljob
names/rolecollapsedmembershipsvalidatedagainstshipped8map. Fewer16MBhoststacks
thanold8worker4Coremap. RuntimejobmapvalidationPENDING.
- OptinCOMproxycacheenabledincombinedpackage(DIRT2_COM_CACHE=1),sanitizerregressionPASS.

VALIDATION:1346x86UnicorncasesPASSwithstrictmath/TSOflags; CPU/SIMD/SEHGCC/Clang
layoutfixturePASS;runtime3CTestPASS(dispatch,memory,workergraph);proxyASAN/UBSANPASS;
full540sourcearchivecompile+nativeSwitchlinkPASS. NoFPSgain/30FPSclaim untilhardware.

BUILD18a21d0286a42d17b409b8dd9e4763ba04d0e270, SHA
4bde22ecde1d9d339fb882827eae523c1f3be4b45a496c5ec5f69197cc24e509,
191280937B. Packagebuild/package/switch-cpu-global;ELF/nmunder symbols.
ManifestinstalledFALSE. FirstFTPstagedenv/README, NROsendtimedout. NOACTIVATION,
old6fb23422stillactive. Retryconnecttimedout. AskeduserreopenFTPkeepawake.
/tmp/dirt2-global-install.py installercontainscredentials,removebeforeend. Staging
remote*.upload-* mayremain;canremoveONLYthosetaskownedpartialstagesafterinstall.
Oldbackupsretained. NextinstallverifiedallstagedthenactivateNROlast,manifesttrueonly
onCOMPLETE. Usertestsunchangedsettings30smenu+2minsameCroatiarace,checkphysics/visuals.

Repro tools/scripts/build_switch_cpu_variant.py --all --compiler clang --no-force-inline
--jobs 6 --output build/switch-cpu-clang. CMakeprimarycurrentlyimports
build/switch-cpu-clang/libdirt2_gen.a. BuildscriptDIRT2_SWITCH_CPU_ARCHIVE override needed
orfullscriptwillselectoriginalGCCarchive. GenericDIRT2_GENERATED_ARCHIVEoptionadded
forfastMacnativeiteration;MaccacheRESETtoempty(sourcebuilds)aftertest.

Macbenchmarkattempt:addedoptinDIRT2_BENCHMARK_FILE GetCmdLineA/Wargumenthook from
shippedexample_benchmark.xml instructions. NativeMacbuildPASS(reusedreadonlycopied
oldMacgeneratedarchive,ABIlayoutunchanged). IsolatedMacsave/settingsrun stayedattract
menu;NOTusableRACEprofile. StoppedPID68176. NootherMacgameleft. NoUIautomationused.
OptionalbenchmarkargNOTenabledinSwitchpackage; donotclaimautomatedbenchmarkworks.
Detailsdocs/SWITCH_PERFORMANCE.md. Source changes checkpointed for hardware comparison.

## CURRENT CONSOLE: CPU COMPARISON INSTALLED/VERIFIED

Build6fb2342237bd3d894df016a92a53b9626fd71b16 installed successfully throughFTP,
fullRETR/SHA256 verified env/README/NRO; timestampedbackups retained. Manifest
build/package/switch-cpu-inline/manifest.json installedtrue. Installer66600COMPLETE.
Priorlogs/envpreservedbefore-install-20261020-222450. Logstillprior438df7fa330s
session; cleanAMexitbuild1acehasnotproducednewsessionlog. ThusQuitresultpending.

Containsperthreaddispatchcachefix + fourchunkmemoryinlineCPUexperiment +cleanAM
exit. CPU/DXVKlowfrequencytraceON, detailed100HzsamplerOFF. User to test30smenu,
2minsameCroatiarace,unchangedsettings,thenQuitandreopenFTP; reportFPS, wheelmotion,
andQuitresult. No measuredFPSgainor30FPSyet. Checksourcecommit19b417f and
variantmanifest/matchingELFsymbols. Credentialsinstallerremovedaftercompletion.

## CURRENT: CPU COMPARISON BUILD READY, FTP TIMED OUT

User asked continue performance. Implemented/tested two changes:
1) Real dispatcher sharedcache had multiwriterpublicationrace (can publish A's
addresswithBfunction). OldactualdispatcherFAILSnew4millioncollidinglookupregression;
newperthread4096slotcachePASS inclcross-threadreregistration andASAN/UBSAN.
Lazyheapallocation64KB/thread; onlyownerpointerTLS toavoidoverflow64KBnative
watchdog/profiler suppliedstacks. No proofthiscausedwheelissue.
2) Opt-in RECOMP_INLINE_GUEST_MEMORY always_inline annotation, preservingTSO /
unalignedfences/math. Limitedvariant recompilesgen0237/0450/0453/0456 usingbaseline
compileDBflags; baselinearchiveUNTOUCHED. f010716F0staticcalls476->63,code4607->8055
instructions(excludesoldhelperbodycost). Not dynamiccounts orFPSgain. Fullsource
CMakeDIRT2_INLINE_GUEST_MEMORY defaultOFF. Varianttools/scripts/build_switch_cpu_variant.py;
CMakebuild/switch-dxvk nowimportsbuild/switch-cpu-inline/libdirt2_gen.a. Buildscript
supportsDIRT2_SWITCH_CPU_ARCHIVE override; withoutit choosesoriginalarchive.

AlltargetedtestsPASS: runtime2CTesttests, dispatcherASAN/UBSAN, memory1million
aligned/unalignedoperationsASAN/UBSAN. Existing8UnicornshardsPASSoutside sandbox
(SIGILLsandboxJIT). Newtests/runtime linkedrealdispatch.cpp,releaseassertsON.
SwitchcrossbuildPASS. SDKstaticTLSsize~4312bytes,below64KBnativebuffers.

Preparedbuild6fb2342237bd3d894df016a92a53b9626fd71b16,
SHAfb4f67f36936a4f609626f82f8532f4f201dad2e482463540f30087808ef1e1d,
167946025B. Packagebuild/package/switch-cpu-inline, matchingELF/nmunder symbols.
Manifestrecordsvariantarchivehash/chunks, installedFALSE. FTP timedoutbefore
anyremotechange. Askeduserreopen192.168.1.148:5000. ConsoleSTILL1ace0cdc(cleanAMexit),
Quitresultpending (useraskedcontinueperfwithoutreportingit). DoNOTclaimnewbuild
installed,FPSgain,30FPS,physicsfixed. Installer/tmp/dirt2-perf-install.py contains
credentials: removebeforeturnend/recreateifneeded. Newbuildretainsfresh+async /
diskcacheOFF, CPU/DXVKlowfrequencytraceON, detailedindirectsamplerOFF. Needtest
same30smenu/2minsameCroatiaracewithunchangedsettings thenFTP comparelogs. Previous
330secondtraceincludes100Hzsampler; previous1acebuildwithoutithasnofreshreportyet,
soABneedsaccountfordiagnosticoverhead. Allworkdocumenteddocs/SWITCH_PERFORMANCE.md.

## LATEST: CLEAN AM EXIT BUILD INSTALLED/VERIFIED

User tested438df7fa: Quit still displayed system error. New evidence in
build/performance/switch-20261020-214628; log ends ExitProcess(0), no NEW
Atmosphere crash (inventory latest01792521523 stillold8d28build). DirectSVC
exit omitted AM notification. Replaced normal Switch ExitProcess/TerminateProcess
with platform::exit_process: execute on SDL main thread, force libnx applet exit
mode1, appletExit installs application-exit callback, invoke it. SendsSelfExit,
waits for OS termination; closes title toHOME instead of hbmenu. No HID teardown
or hbloader chainload over live GPU threads. Guest-return also useshelper and
keeps eventpump running until callback. SDK disassembly matches upstreammode1.
Hardware result PENDING; do not claim quitfixed until tested.

Build1ace0cdc8f1b675f4e563645caee1096088dd529, NRO SHA256
dbe58f67f350702c0733bed064684210afa20b58003716c57f34250174f95255.
Packagebuild/package/switch-clean-exit; matchingELF/nmunder symbols.
Installer71223 COMPLETE; env/README/NRO fullRETR/SHA256verified. Manifest
installedtrue. User to launchthenuseQuit(shorttest sufficient), reopenFTP. DetailedindirectsamplerOFF nowtracecaptured;
lowfrequencyCPU/drivercountersON. Fresh+async/cacheoffretained. OriginalDX9untouched.

Worker264 racecontexts aggregatedworker-contexts.json; genericqueuewait00BF1750
plus009EB920,010D3A20,010716F0,01091060,0107B2E0 substantialinclusivecontexts.
Not selfCPU/PCsampling; no FPS/physics fixproven. Need identifyfunctions/tasks
fromgeneratedcode/referenceanalysis beforeperformancechanges.

## CURRENT CONSOLE: QUIT REPAIR / INDIRECT PROFILER INSTALLED

Build438df7fa8aaeae87cf14884513834ecd33475718 fully uploaded and RETR/SHA256
verified at192.168.1.148:5000; env/README/NRO replaced with timestamped backups.
Manifest build/package/switch-quit-profile/manifest.json installed=true.
User to run DiRT2 (dirt2-dx11), menu30s/race2min then use in-game Quit and
reopen FTP. Quit hardware result and hot worker context remain pending.
Physics/wheel cause remains unresolved. Original D3D9 untouched.
Temporary credential installer removed after completion.

## CURRENT: QUIT REPAIR BUILT; FTP INSTALL BLOCKED

Latest user reported Quit crash + wobbling wheels in recorded grid introduction;
completed 30s menu/2min race profile. Evidence downloaded to
build/performance/switch-20261020-203708 (logs, matching Atmosphere crash, video).
Quit crash matches installed 8d28b243 ELF: ExitProcess(0) -> libnx cleanup races
SDL main-thread hidGetTouchScreenStates -> UserBreak 0x1159. Switch ExitProcess /
TerminateProcess now use svcExitProcess, matching existing DX11 guest-return path.
Cross-build passes; hardware validation pending.

NEW BUILD 438df7fa8aaeae87cf14884513834ecd33475718, SHA256
fff4be07c09d868a8c1716021df478c0e65fb38e3c49ced3b09ffbd318db0dc3
in build/package/switch-quit-profile/; matching ELF/nm archived under symbols/.
NOT INSTALLED: installer timed out connecting before any remote write. Console
still has 8d28b243. Asked user to reopen FTP at192.168.1.148:5000. No credential
should be committed. Temporary FTP scripts containing credentials were removed.

Race profile185–306s: median11.6FPS,86.185ms/frame. Guest tid264 entry01161370
uses93.3% of one core; main26039.45%,worker34018.5%. GPU/CS waits0,
Present0.057ms/frame, Map0.533ms/frame, compiler stable1361/1361. GPU queue idle
is not occupancy. Identifies saturated worker, not actual work yet. New opt-in
DIRT2_PROFILE_INDIRECT=1 samples active indirect-call contexts100Hz without
suspension; reports inclusive wall contexts for threads above20%core. Not PC/self
CPU sampling; extra diagnostic overhead requires profile-off final comparison.

Video2026102020353800...mp4 shows start-grid wheel motion at14–15FPS. Physics
cause NOT established/fixed. Mac12FPS comparison launched with isolated copied
save/settings; relative game path failed; absolute launch reached12.2FPS intro.
CUA bundle comparison gave black screen (one screenshot call stalled42min),
so no usable Mac race comparison. Test process17721 terminated; no verified
physics conclusion. Do not call wheel issue fixed or claim30FPS achieved.

## CURRENT CONSOLE: PRIMARY DX11 PERFORMANCE PROFILER INSTALLED/VERIFIED

Checkpoint41d7774 committedallinitialwork inclDX11primary/DX9fallback andhalfresXML.
User CONFIRMS640x360works. Newprofilerbuild8d28b243d48a3b802312548bfa9dd2d9a810a857
FULLYINSTALLED/RETRverified env/README/NRO, manifesttrue in
build/package/switch-performance-current/. SHAabd8c14e55ea649500dd5fa6bb1bd9ccf72db78dbbefdf5a8f3eb4f74ce4d6e7.
MatchingELF/symbolsarchived. Latestpreinstalllogs preservedunderpackagebefore-install-*.
ConsoleappnameDiRT2 indirt2-dx11folder; originaldekoentryuntouched. Userinstructed
menu30s thenrace1–2min unchangedsettings, close/openFTP forCPU/driverlogs. Await.
Do NOT claim30FPSachieved. Useradditionallyreportsresolutionreductiondoesnotchange
FPSandGPU30–40%, stronglyCPUbound; newtraceaimstoidentifyCPUthread/driver/waits.

Profileenabled: guestCPUticks10s+affinity/queryRC; DXVKcounters/Present+allnativeMap
wall andshadowcopyMiB5s. CPUonlyguestthreads; queueidleNOTGPUshaderoccupancy;
concurrentwaitcountersnotexclusivephases; copybytesnotcopytime. No threadpauses
ornewGPUwaits. Brightnessfresh+async/cacheoffdisk retained, exposureCSVoff.
StackPointerArray<=16ON (largeheapfallback/getterzero/moveASANtested). COMlookup
cacheOPTINONLY viaDIRT2_COM_CACHE=1; defaultOFF inclcurrentconsoleprofilebaseline.
Mac4runABcompleted,noerrors: FPS214.55/208.05/202.35/203.85 forcache0/1/1/0;
sampledlookup4.95/2.71/2.63/3.47%. CPUworkreduced butNOFPSgainestablished.
MacnativeDXMTdistinctfromNVK; docs/SWITCH_PERFORMANCE.md fullrepro/limits.

Allbuilds/tests/bench/atos/transferscomplete; credential/tempbenchmarkscriptcleanup
completedbeforeturnend. Nextretrieveprofilerlogs+config fromFTP and analyzeCPU
saturation,Map/copybandwidth,CSwaits,compiler/submissionpressure BEFOREfurther
pixelreductions/unsafeTSOchanges. No affinity/clockchangesmade; disassemblyshows
libnxpthreadalreadybroadensallowedcoremask. Newperformanceworkseparatecommit
from41d7774; sourcecanbereviewedagainstcheckpoint. Shaderpreload remainsOFF.

## USER STEERING: RESOLUTION DOES NOT AFFECT FPS; GPU LOAD30–40%

User reports resolutionreductionunchangedFPS andGPUload30–40%, stronglysupporting
CPU-sidebottleneck. PrioritizeCPUengine/bridge/driver-submission/synchronization;
do NOT spendnextstepreducingpixels. Explain exact CPUthread/stallcause stillneeds
newprofile. Installer53955stillrunning; poll beforetellinguserlaunch.

## LATEST: HALF-RES CONFIRMED; PROFILING BUILD INSTALL IN PROGRESS

User confirms640x360 works andFTPready. Requestedallworkcheckpoint is41d7774;
DX11primary/DX9fallback preserved. Newperfworkseparatependingcommit.
Currentprofilebuild8d28b243d48a3b802312548bfa9dd2d9a810a857,
NRO166876969B SHAabd8c14e55ea649500dd5fa6bb1bd9ccf72db78dbbefdf5a8f3eb4f74ce4d6e7.
Packagebuild/package/switch-performance-current/DiRT2-switch.zip verifiedagainst
actualELF/NRO andZIP. MatchingELF/symbolsarchived. CacheOFF (optin1 only),
stackPointerArrayON, performance traceON/exposureCSVoff, cache-diskOFF, fresh+asyncON.
Installer session53955 running fullatomic/fullRETR; preserve latestlogsfirst.
Do notclaiminstalled untilsessionreturnsCOMPLETE+manifesttrue. Nextuser launch
DiRT2 in dirt2-dx11 folder, menu30s/race60–120s,samesettings, thenFTP/logs.
CPUaccounting +DXVKwait/draw/compiler/mapbytecounterswillguideactual30FPSeffort.
No30FPSclaim. NoFPSgainprovenonMacAB; COMcachedefaultOFFallplatforms.
Afterinstall remove/tmp/dirt2-profile-install.py (credentials) andbenchmarktemp
scripts. UpdateHANDOVERmanifest; commitnewperfworkseparatefrom41d7774.
AllMacbenchmarkprocesses+atoscompleted. FinalcompileSwitchPASS/currentASANtests
PASS; preparepatchreversechecksPASS; noactivebuilds exceptinstaller.

## PERFORMANCE UPDATE: CHECKPOINT41d7774 SAFE; AWAIT FTP/HARDWARE PROFILE

Do notclaim30FPS or performancefix. Usergoal30+ notyetachieved.
Allinitialwork+DX11primary/DX9fallback+halfresfix committed41d7774. Halfres48d03
installed/fullverified beforepromotion. Asyncquestionpendingwhether640x360works
andFTPopen. LastFTP refused; no profiler installed. Currentconsole remains48d03.

MacABfour50s runs samebinary/cache0,1,1,0 completedexit0, no unknownproxyerrors.
Median15–30sFPS214.55/208.05/202.35/203.85; to_hostsample4.95/2.71/2.63/3.47%.
CPUworkreduced but NO FPSgainproven; cache nowOPT-INonlyDIRT2_COM_CACHE=1 (default
OFFallplatforms). StackPointerArray<=16 avoidsheapalloc; largefallback/getterzero/
move testedASANUBSAN. Cache100krandomretirement/reuse/collisionASANUBSAN PASS.

Profilerexpanded: guestCPUticks10s +affinity/queryRC; nativeDXVK15counters/Present
wall5s; Mapnativeelapsed+shadowcopyMiB perframe. Nothreadpause/noGPUwaitadded.
NativeCPUtimeexcludesdriver/audio/compiler; queueidleNOTHWoccupancy; copybytesnot
memcpytime; waitcountersconcurrentnotexclusivephases. Nativepthreaddisassembly
confirms__syscall_thread_create alreadysetsALLallowedcoremask; noaffinityfixmade.

Latestbuildsession7228completed; newpackage session44764 produced earlierbefore
lastconfigchanges? Mustensurecurrentbuild/NROSHA package freshlymatches.
Rootbuild/switch-dxvk currentlycacheOFF-bydefault+stackarrays+fullprofiling.
MacbinaryusedAB defaultcacheON butexplicit0/1constant; source nowdefaultOFF and
Switch-onlyprofilerextra may requireMacbridgefreshrebuild ifcomparingfuture.
Allbenchprocesses/slowatos finished; scripts /tmp/dirt2-mac* no creds canremove.
/tmp/dirt2-profile-install.py STILL containsFTPcredentials and pointsOLD5c66
package; UPDATEto switch-performance-current beforeinstallorDELETEbeforeturnend.
Fullprofilingcurrentpackage shouldremaincacheOFF forbaseline; cache can be enabled
later viaenv withoutNROupload. Preserveold5c66symbols/packageasprovenance.
Docs/SWITCH_PERFORMANCE.md containsfindings/reproduction/limits. Next finishfresh
package/symbolchecks, commitprofiling/CPUworkseparatelyfrom41d7774, ifFTPavailable
atomic/fullRETRinstalllatest withpreservedlogs. Otherwisedeletecredentialscript,
reportinstallblockedbyFTPandaskuser runwhenready. Nohardwaretimingsavailableyet.

## CHECKPOINT COMMITTED; PERFORMANCE PROFILER + FIRST CPU OPTIMIZATION IN PROGRESS

Checkpoint41d7774 includes ALL accumulated work, DX11primary/DX9fallback, halfresXMLfix.
Userfeedback latestasyncgraphicsclean,11–13FPSrace/20menu. Halfres48d03 install
alreadyfullverified. Laterprimary521fc375 built/packageNOTinstalled.

New low-frequency Switch profiler built5c661e1f0f43fbb9ba7b28514639f583164f44e8,
SHA363fe22c8edff33acf243de225478ee219986e1c17f122f640138cedde0478d3,
packagebuild/package/switch-performance/ primaryDiRT2NACP +halfresfix. CPUtick
accountingguestthreadsviaInfoThreadTickCount every10s, no pause/stackreading;
DXVKnativeexport15counters plusPresentwalltime every5s. GPUqueueidle NOThardware
occupancy; CPUlogger excludesnativecompiler/driver/audio. Sourcechangesuncommitted
separatefrom41d7774. Perfexportpatch0005 preparedapplied/nativearchivebuilt.
InstallattemptconnectionREFUSED; no mutation. AsyncquestionpendingopenFTP/check
640x360 works. Script/tmp/dirt2-profile-install.py containscredentials; DELETE
whenfinishedorwaitingturn. Itpreserveslatestlogs beforeatomic/fullRETRinstall.
Do notclaim profiler installed! Matching5c66ELF/symbolsarchivedbeforelaterbuilds.

Mac75secondisolatedmenu/intro run COMPLETE viaexplicituserauthorization; own
benchmarkdata build/performance/mac-menu. SandboxedSDLnodisplays; escalatedrun
workedM1Max; exitcode0. CurrentMacbinaryoldOct19, nativeDXMT so Switch-specific
bottleneckinferenceslimited. ExistingAppleprofilerallthreadPCsample5windows30–70s.
functions-summary.json resolvedwithnm/c++filt: outside-main43.37%, to_host4.558%,
lookup1.24%, BinkWait1.238%, f_011D5F30~0.98%, IASetVertexBuffers~0.966%, vectorUAV
resize~0.764%. Percentdenominatorallnonkernelthreads; top40 truncation; NOTframewall%.
Slowatoschild remains /summarize.py session13681; functions-summary usingnmcompleted.
/tmp/dirt2-mac-profile.py no creds; completedownprocessMacgameclosed.

Firstoptimizationinprogress: sharedCOMto_host perthread64slotcache, atomicgeneration
invalidated atbothforget+hostreuse; registrymutexauthoritativeonmiss, no cachednull,
no COMrefcountchanges. DIRT2_COM_CACHE=0 supportscomparison. HostASAN/UBSAN random
100kcollision/retirement/reuse regressionPASS. Crossbuildsession55459 running.
Thiscachechange NOT included5c66baselineprofilerpackage. Needfinishbuild, separate
optimizedpackage, noinstallbeforebaselineprofile/userclarification. CanrunMacsame
scenecomparisonafterrebuildingbridge; do notclaim30FPSachieved orpromisespeedup.
Originalobjective still30FPS requiresactualhardwareprofilethen targetedrepairs.

## DX11 PRIMARY PROMOTION + HALF-RESOLUTION FIX; CHECKPOINT BEFORE PERFORMANCE

User explicitly requests commit ALL current work, promoteDX11 primary, keepDX9,
then work toward30FPS. First fix/upload0.5x before rest: DONE atomic/fullRETR
install48d03d71a782b605abc683bf4a4ba50dbbd0431d, SHA1db7fb4440b2ff691a6e3ba8efb29a8415733c369013d0784c953eac6dcab7a4.
Console still title DiRT2DX11prototype until next promoted build install.
640x360 was advertised and lifted360min already compiled, but game asset
system/hardware_settings_restrictions.xml clampsheight<=479 to640x480. New
Switch-only runtimeprivateVFSoverride changes exactly3heightattributes to360/359.
Originalasset/Apple unchanged. HostASAN/UBSAN realasset roundtrip/malformed PASS.
REA skillread; open_binary identifies Steamv1.1 exe49b1e00e...; Hopperstartup timed
out, sessionclosed. No successfuldecompile reliedon; XML decisive.

User confirms latestasyncDX11 graphics CLEAN, but menu20/race11–13FPS at0.75xlow.
New baseline build/package/switch-dx11/session-20261020-195601/ beforeupload:
exposureMapmedian0.014ms inrace (p950.016–0.018ms), frame lag1. Exposurewait is
NOT thecurrentbottleneck! Need CPU/GPU profile. Still preserve correction.

Promotionprepared: defaultDIRT2_SWITCH_DXVK=ON, switchpresetisolatedNVK/buildswitch-dxvk;
switch-deko explicitOFF/systemSDK; defaultpackage_switch.sh nowDX11primary
fresh+async/cacheoff/watchdog, noexposureCSV. Legacy scriptpackage_switch_deko.sh
retainsallDX9code/shaderpack. NACPprimaryDiRT2/fallbackDiRT2DX9fallback. Paths
remain dirt2-dx11 toavoidsettingsmigration. README/docsupdated.
Primarybuild/package/checks PASS: build521fc375f9398a2d73e344309de5f4bdb188ec9f,
SHA0cb92c5331af2f8d87938d1e7f7029f6c066b166565f4ce16c3248bb6a349342.
PrimaryZIPbuild/package/switch/DiRT2-switch.zip, fresh+async/cacheoff/watchdog,
CSV OFF. Console stillhalf-res48d03 pendingmenuconfirmation.
Next commitALLsourceinclpriorwork (userauthorized;
no generatedgameassets/buildoutputs/credentials/cachefiles), THEN performance work.
Do not mix performancechanges into checkpoint. No subagents unlessskillexplicitly
requires; current sequentialwork fine. Credential scripts deleted; no running tools/transfers. OriginalDX9source/in-flightotheragenteditspreserved.

## PERFORMANCE BUILD INSTALLED — AWAIT HARDWARE RACE COMPARISON

## PERFORMANCE FOLLOW-UP: USER REPORTS NO OSCILLATION, RACE 15 FPS

User tested df88 fresh-exposure build: no brightness oscillation this session,
but race ~15 FPS (not menu). Initial FTP connections refused, then permission review timed out (not a safety
rejection); allowed retry succeeded. Baseline retrieved BEFORE replacement to
build/package/switch-dx11/performance-20261020-191921/ (log/CSV/env/config/SHA). Do not claim measured
readback bottleneck: newest blocking exposure readback is a plausible contributor.

Installed opt-in --fresh-exposure --async-exposure --exposure-trace package.
Switch-only known exposure callsite polls newest eligible staging pixels with
DONOTWAIT, up to2 presents old, same immediate context. Newest completed wins;
if none ready, original fresh latest-pixel blocking path stays. Non-busy errors
propagate. Successful actual native map still paired with Unmap. No cached/faked
brightness pixels; no general D3D11 map changes. Runtime confirmation required:
brightness could regress with bounded delay, FPS improvement not proven.
ExposureCopy now includes context for candidate filtering. CSV adds map_ms to
measure total selection/Map cost; no extra clock calls on non-exposure maps.

Build/local existing ASAN checks/package ZIP/config verification PASS.
Prepared build8dc98898d8bbc46637f5ca528304d97263431f4a,166856489B,
SHA5e8724a9b717cba44149488b63d13479a8c51f6dc5098914a386ed2d4597f92e.
Package build/package/switch-dx11-async/; matchingELF/symbols archived there.
Previous df88 package manifest preserved as historical installation.
Async build COMPLETE: env/README/NRO atomically installed with backups and full
RETR SHA verification; async manifest installed=true/full_readback_verified=true.

Baseline analysis.json:7628 successful samples,99.79% rerouted; original ring
age median406.76ms versus fresh7.203ms. Actual copy frame lag1 for7612samples.
Racewindow frames6000-6600 median59.478ms copy-to-read/p9587.101; later
6900-8401 median23.514ms. FPS intervals13.5,13.0,17.6 then21.1 steady; output720p.
These ages INCLUDE timing between CPUcopyqueue/Map, not direct GPUwaitmeasure.
No running tools/builds/transfers. Credential scripts deleted.
Next user: same race/settings, compare FPS and brightness, then reopenFTP. Keep disk caches OFF and shader
preload OFF for comparison. Existing resolution/shared-save fixes unchanged.

## BRIGHTNESS BASELINE RETRIEVED/ANALYZED BEFORE ANY INSTALL; LONGER FRESH TEST BUILD

User explicitly asks collectbrightness evidence beforeinstallation. DONE read-only
FTP download baseline-20261020-182303/ under exposure-latency-20261020/: current/
prevlog, current/prevCSV, env, nativeDXVKlog, config and inventory. Checksums/size
verified for logs/CSV. Analysis brightness-analysis.json. Native Presenter1280x720
confirmed2times. CurrentCSV2964 successfulRGBA16F READs/16resources; no negative
or nonfinite pixel components. Rangechannels inJSON, don't label them calibrated
screenbrightness. Median ringREUSE16frames/474ms current,16frames/687ms prior; NOT
measurementofcopy-to-readage (oldtracehasnocopystamps). FPSslowersections14-35,
latersteady23.6. Sessionlasts>500s butCSVlimitedfirst120s (lastflush115s), so worst
later oscillations may be outside trace. Explicitly told userthislimitation.

Extendedtrace env DIRT2_EXPOSURE_TRACE_SECONDS=600 (bounded3600) andpackageREADME.
Freshlatencyexperiment stillopt-in; no confirmedbrightnessfix. ShaderwarmupOFF in
comparison. Crossbuild +parser/size/VFSASANchecks/diffcheck PASS. FreshELF/symbols archived; packages/env verified; normaldekoNRO unchanged.
Build df88aed375f3fa780e982c2366e05b8f6b3a44f2, SHA29653611547f6cdb55b8d00bd57e934b634d738de190a5463c1727932f6deea1.
FULLatomic/fullRETR install COMPLETE: env/README/NRO verified; manifest
installed=true/full_readback_verified=true, baseline_directory recorded. Next user
launchsameDX11prototype, reproducebrightnessmenu/race1-2min, thenFTP. Compare
actual_resource vsresource andoriginal/actualage inextendedCSV; reportFPSand
brightnessresult. Shaderwarmup remainsOFF, alternativeZIPnotinstalled.
Noactivebuilds/tools/transfers; credential scripts deleted.
Baselinealreadysecured; no further permissionneeded. Credential scripts removed. Reproducible analysis script saved alongside baselineJSON.
Both alternativepackages regenerated withnewNRO, warmupZIP remainsnotinstalled.

## USER CONFIRMS RESOLUTION FIX; EXPOSURE UNPLAYABLE: FRESH-READBACK COMPARISON READY

User confirms e087 fixes resolution. Now asks shader precompile and reports
brightness oscillation unplayable; suspects lowFPS increases exposure delay.
Keep physical720p fix/shared saves/cache-off. No further FTP collection this turn
(user still testing; await ready before installation).

Local shader preload implemented, opt-in only. package_dx11_shaders.py deduplicates
2768 game DXBC +8 generatedUI into1117 unique shaders,4653520B read-onlybundle,
SHA8e18a130819fe3fda493feeadf54ae87671c6814a2085106c79a0e8dbc9b9307.
Native D3D11 Create*Shader uses actualSwitch compileoptions, registers DXVK GPL
libraries; commonshader/pipelinemanager caches retain them. Waits nativecompiler
queue; loading presents showDXVK HUD. No diskshadercache writes. Startupwatchdog
now120s WITHOUT PROGRESS; shadercreation/completedtasks resetdeadline. Newnative
stats export patch0004, aggregatearchive regenerated. AllCPU parser/malformedfile
ASAN/UBSAN tests PASS. Crossbuild PASS. NOT hardwaretested. Perlaunchwarmup, NOT
persistentprecompiledGPUbinarycache; state-specificpipeline variants maystillcompile.
Packaged separately build/package/switch-dx11-preload/ (notinstalled) so current
brightnesscomparison leaveswarmupOFF. --shader-preload asset andenv opt-in.

Exposureexperiment: CopyResource/CopySubresourceRegion hooks track time/frame for
1x1 RGBA16F CPUread staging copies from1x1 RT, bycontext. Optional
DIRT2_DX11_FRESH_EXPOSURE=1 reroutes ONLY MapREAD flags0 fromknownFAF55Ccallsite,
immediatecontext, to latestqueued matchingstagingpixel. Shadowtracksactualnative
mappedresource so Unmap ispairedcorrectly. Generalmaps unchanged. This is a
GAME-SPECIFIC latencyexperiment, notgeneralD3Dtranslationsemantics andnotyetfix.
No extraGPUcopy/readbacks, but blockingnewerMaptradesGPUoverlapforfreshvalues.
CSV recordsoriginal/actualresource,copyframe,age_ms to quantifydelay. Doesnot
clamp/gammaadjustreadvalues. Runtimecopies invalidatedwhenproxyreleased.

Current top package --fresh-exposure --exposure-trace --no-disk-shader-cache;
shaderpreloadOFF to isolatebrightness. Fullbuild +bundleparser/size/VFS testsPASS.
ELF/symbols archived; bothpackages verified; originalNRO unchanged. Newa025663
NRO166852393B, SHA3d84030658c22353392ba4d9729188cb5fbfb9d5c99fbb41cf0ddb1268901a8c.
Async request pending: closegame/openFTP. Installfreshcomparison whenready; getbaseline
latestlogs/exposurebeforeNROreplacement. Snapshotpreviousinstalledmanifest in
exposure-latency-20261020/. Noactivecredentials/FTPtransfers. Installer was prepared but not run; temporary
credential script deleted while awaitingFTP. Must retrieve baseline dirt2.log,
exposure.csv and env.txt before atomicNROlast install. WarmupZIP NOT installed.
Bothpackage manifests installed=false; currentconsole stays e087 (resolutionfixed).

## LOWRES ROOT CAUSE CONFIRMED: SHARED 256x256 SPLASH BECAME VULKAN OUTPUT

Latest lowres-result-20261020-175505/: three new SwitchJPGs show entire output
including nativeDXVK HUD huge/jagged (not just game graphics). Log: initialloading
HWND created256x256, then shared neonclass main800x600; requested D3D backbuffer
1280x720 but Presenter ACTUAL Buffer size256x256! NoResizeTarget/ResizeBuffers in
this run. Last resolution repair didn't fix this independent initialoutput bug.

platform::create_window shares one SDL window on Switch. First splash creates its
NWindow256x256. SDL Switch CreateWindow calls nwindowSetDimensions(window->w/h),
but Vulkan SetWindowSize path is a no-op (onlyEGL handled), so later fullscreen/
window metadata changes never repair NWindow extent. DXVK Presenter follows
surface currentExtent (256x256), downsamples whole completedframe/HUD there,
then VI stretches to1280x720. Solidblue preflight missed this spatial issue.
Source locations: SDL_switchvideo.c CreateWindow/SetWindowSize in configured
build/deps/switch-work/SDL3-src; DXVK Presenter::pickImageExtent and loggedextent.

Repair: SwitchDXVK-only create physical shared SDL/NWindow at1280x720 even for
splash; platform set_window_size holds it at1280x720; ResizeTarget nativeDXVK call
uses physical1280x720 while preserving original virtualrender target for buffers.
Guestmode sizes/selected renderbuffers remain independently scalable. No SDK or
Apple/deko edits. Must confirm next Presenter Buffer size1280x720 and HUD sharp.
Build/package PASS, preserve cache-off/shared saves/exposuretrace.
New build e087659f27cfd9393838daa970eba96e6bd8d1c4, NRO166844201B,
SHA256 04da10a44282533d720717bde32a7bcc7b05ed41da186762112e4384a9c47010.
Matching ELF/symbols saved; ZIP/env/NRO and unchanged deko SHA verified.
Render-size and VFS host ASAN/UBSAN regressions PASS. Atomic/full RETR install
COMPLETE: env/README/NRO verified; manifest installed=true/readback=true.
No active tools/builds/transfers. Credential scripts removed. Await user to launch
at1x and check crisp HUD/menu; thenFTP log/exposure to confirm actualoutput720p.

ExposureCSV2751 successful READs, allRGBA16F, pitch8,16 resources, zero negative/
nonfinite pixels. Invalid32F guard does NOT explain oscillation; prioriOS8-deep
ring theory notprovenhere. Channelranges in analysis.json. G oscillates earlythen
converges near1 later. Brightnesscause remainsunconfirmed; no arbitraryclamp.
FTP install complete; temporary credential scripts removed. Imagesretrieved.

## DX11 MENU EXPOSURE OSCILLATION + LOW/UNCHANGING RESOLUTION; FIX/TRACE BUILDING

User sees iOS-like brightness oscillations in Switch menu, also very low resolution
regardless setting. Retrieved visual-result-20261020-174116/: env/log/config.
Config saved1280x720. Log initial swap chain1920x1080, later setting change calls
ResizeBuffers(2,0,0) -> fixed window1280x720, rebuilds quarter RT320x180 (previous
480x270). FPS~15-29 menu. No old invalid-luminance logs. iOS commits5b9821b/dd5b485
claimed delayed8-deep ring autoexposure; that prior inference is NOT proof here.
Old guard identifies any16-byte READ asRGBA32F, misses padded pitches and can alter
unrelated resources. Now metadata identifies staging1x1 float textures; guard only
actualRGBA32F, size>=16. Small diagnostic logging no longer reads16 bytes from
shorter maps. New optional DIRT2_LOG_EXPOSURE records raw pixels/format/pitch/Map
HRESULT/flags/frame/time, batches128 samples or5s into exposure.csv for120s.
Failed busy maps rate-limited perresource/frame; no extra GPU reads/waits. Must
analyze resultingCSV before claiming oscillation rootcause/fix.

Switch-only DXGI mode enumeration/closest mode now uses platform render scales
0.5/0.75/1/1.25/1.5/2x720p. Desktopcoordinates720p. Tracks successful ResizeTarget
request as virtual render size, resolves subsequent zero ResizeBuffers dimensions
from target rather than immutable Switch window. Explicit dimensions win; other
platforms retain native window fallback. Existing save sharing/cache-off retained.
ASAN/UBSAN test_render_size.cpp PASS. Full build/package PASS. Build 3c2d1483de0e86f202e313510aa098e49def5ba6,
NRO SHA256 b02c27782e5d2c691ae7d3bff28fe11e7859a6910213d78e2bcd90122a2d747f (166844201B).
Matching ELF/symbols saved; ZIP/NRO/env verified and normal deko NRO unchanged.
Atomic/full RETR install COMPLETE: env/README/NRO all verified; manifest
installed=true/full_readback_verified=true. Credential scripts deleted; no active
builds/transfers. Latest source and executable3c2d148 match.
Next user: choose1x vs1.5x/0.5x to check actual resolution change; leave menu
with brightness oscillating30-60sec, then reopenFTP. Retrieve exposure.csv and
log/config. No claim oscillation fixed until evidence/test.
FTP transfer finished; temporary credential scripts removed.

## CACHE-OFF GAME REACHES MAIN MENU; SHARED SAVE SUPPORT BUILDING

User confirms603231 with disk caches disabled REACHES MAIN MENU. This supports
cache-write contention as the startup blocker, but exact filesystem cause remains
unproven. Keep --no-disk-shader-cache + --startup-watchdog in future packages.
User now requests same save as original Switch version. Implemented VFS save-tree
redirect (set_save_dir/save_dir/is_data_path) and DXVK-only startup selection of
sdmc:/switch/dirt2/C/Users/Player/Documents/My Games/DiRT2/savegame. Read AND write
share original tree; prototype graphics/settings/logs remain separate. No copying
saves between trees. Child paths redirected; sibling settings and other drives
unchanged. Kernel file mkdir helper recognizes external save tree for new profiles.
ASAN/UBSAN host VFS regression covers shared save subtrees, default mapping,
sibling paths/normalization, other drive, boundary checks, game overrides: PASS.
Build/package and VFS ASAN/UBSAN PASS. Build aba1bbfae43255d162f1219aa60d391bf48cf663,
NRO SHA256 1840f944f2e90c29c09fd6c12addb853d89123f82cce7382a5999d375e0f3bda
(166840105B). Matching ELF/symbols saved. Backup of23 original
save files completed in original-saves-backup-20261020-173432/.
Successful cache-off log also retrieved there: no shader-cache path confirmed,
first Present success at28.606s, main menu confirmed by user. This is stronger
evidence cache writes blocked startup; exact SDK/filesystem mechanism unproven.
Atomic/full RETR installation COMPLETE; env/README/NRO all verified.
Manifest installed=true/full_readback_verified=true. Original saves not modified
by installer; both game versions now share their save tree. Credential script
deleted; no active builds/transfers. User should launch same DX11 prototype and
confirm previous profile/progress loads.

## ORIGINAL STALL CAPTURED: GAME LOG WRITE + DXVK CACHE WRITE; CACHE-OFF ISOLATION

603231 native watchdog WORKED: after120s without Present requested UserBreak
2345-0049, new report01792509728. Module ID matches603231.40 threads captured.
Evidence startup-result-20261020-171916/ includes matching installed manifest and
symbolicated.txt (all threads from archived ELF). Game0429 is in fsFileWrite via
rt::log inside counted CreateTexture2D; last flushed message floatRT1x1 at18.510s.
Shader writer044d also fsFileWrite via util::File::append/writeShaderBinary.
GPU submission/completion/CS/compiler workers idle in condition waits, main SDL
pumps events. This supports investigating I/O before GPU, but one snapshot alone
does NOT prove FS deadlock or broken GPU. Pinned libnx FS sources audited:
source-audit/libnx/fs.c/fs_dev.c. FS already uses session pool (3 sessions).

Next experiment changes ONLY env/README (same603231 NRO): DXVK_SHADER_CACHE_PATH=
(empty, no HOME/XDG fallback defined on Horizon) -> DXVK CacheDisabled, and
MESA_SHADER_CACHE_DISABLE=true. Native shader compilation remains enabled in
memory; renderer/watchdog untouched. Full upload not needed. Must verify next log
says no shader-cache path/disabled, not Created cache file. This is an isolation
experiment, no root-cause/fix claim yet. Existing disk cache files preserved.
Packaging ZIP/manifest reflect cache-off config; config installed/fully readback
verified. Same previously SHA-verified NRO (remote size rechecked), no binary upload.
Captured game-startup.dxvk.bin641925B/lut24356B preserved with evidence.
Credential scripts removed, no active builds/transfers/agents. Await user launch
of DiRT2 DX11 prototype and FTP after result.

## WATCHDOG DETACH STARTUP CRASH IDENTIFIED; NATIVE THREAD REPAIR BUILDING

New evidence startup-result-20261020-171011/: a9cdd4 never enters guest.
New Atmosphere01792509218: NRO base0xd4942d000 inferred from exact watchdog
return address. Main stack: start_dx11_startup_watchdog+0x84 -> std::thread::detach
-> std::__throw_system_error -> terminate -> abort -> __libnx_exit -> hbl unload.
Watchdog thread instruction-aborts at svcSleepThread+4 after NRO is unmapped.
This is our newly introduced watchdog bug, NOT evidence about original GPU hang.
Disassembly pthread_detach unconditionally returns88 (ENOSYS), even with hooks.
C11 checks passed; stale DXVK log is prior game run and must not be treated fresh.

Replaced detached std::thread with native libnx Thread and page-aligned static
64KB backing. Both threadCreate/threadStart checked; failures log/disable watchdog
without throwing. Thread/stack persist until process exit; successful Present
lets thread return safely. No pthread_detach dependency in watchdog. Build/package PASS, matching ELF/symbols saved. New build 603231fc9b584e79730ad4425553df47588eb4f7
SHA256 f5827cda5a51350690b99dae9823676a4ee417a0eb9e547f380e83077eafa037.
Watchdog object undefined symbols checked: native thread API, no std::thread/detach.
Normal deko NRO SHA unchanged. Atomic/full RETR install COMPLETE, all env/README/NRO files verified.
Manifest installed=true and full_readback_verified=true. Credential scripts deleted. Next: user launches real game and opens FTP
after result. Full-game original GPU stall still unresolved.

## STANDALONE GPU/AUDIO/CLEANUP PASS; RESTORING FULL GAME WITH STARTUP WATCHDOG

Latest FTP evidence: build/package/switch-dx11/cleanup-result-20261020/.
92915 probe passes C11 ABI, OpenAL context, RGBA8 and RGBA16F clear/copy/readback,
first Present and GPU completion. Both raw readbacks saved. Final PASS at5.056s,
zero outstanding native NV references, former transfer range recovered at5.070s.
No new Atmosphere crash report; user confirms game closed/FTP ready. The leaked
COM diagnostic cleanup repair is hardware verified. Full game's original freeze
remains unresolved; standalone pass does not establish game correctness.

Real game build a9cdd47703d5a5b6398c01df2a55477f2a3c7321 (166836009 bytes),
SHA256 ef64394b7be66798317efeba8412fdffc37dc4d53f42eccccde0f77424e431aa.
Restoring real game binary with preflight disabled, fresh game-startup cache,
pipeline warmup/record disabled, startup watchdog enabled. After120s without a
successful Present it requests an Atmosphere report, without taking log locks.
First successful native Present disarms it. System-wide hangs can prevent it.
Full-game guest return uses svcExitProcess rather than returning live guest/GPU
threads to a same-process launcher. All changes gated to DXVK Switch prototype.
Installed all three files atomically with full RETR SHA verification; matching
ELF/symbols archived. Ready for user to launch full game via title takeover,
wait up to120s if blank, then reopen FTP after result. Original deko build/folder
untouched. Credential scripts deleted. See latest package manifest for build/hash
and installed/readback verification status; older headers describe prior runs.

## NATIVE PROBE PRESENTS; SPHAIRA RETURN CRASH CLEANUP REPAIR INSTALLED/VERIFIED

User says crashed, FTP ready. native-result-20261020/ contains log, first raw
RGBA8 readback and NEW crash01792507283. Native probe917238 initialized audio/
GPU, cleared/copied/read back RGBA8, presented blue, waited GPU completion and
reached SDL teardown in5.064s. New crash MODULE IS SPHAIRA, not probe! Exception
Address0xdebb3000 equals probe's logged8MB GPU transfer allocation. Launcher
heap reset hits still-protected GPU pages after NRO return (same process).
RGBA8 is uniformly(64,127,191,255), VALID within D3D11 FLOAT->UNORM0.6 ULP.
Old strict expected128 green caused false failure. Float Map0x887A000A is normal
WAS_STILL_DRAWING, not evidence of corruption. Failure path leaked texture/view/
readback refs, retaining device; earlier run_test early returns also leaked all
COM/window objects. This is a CONFIRMED DIAGNOSTIC HARNESS BUG; don't claim the
full game's original hard freeze has been explained or fixed by this alone.

Fixes: scoped std::unique_ptr COM/window owners on every early return, bounded
busy-Map retry10s, exact allowed UNORM pixel set per0.6ULP (not blanket +/-1).
ASAN/UBSAN CPU test accepts actual captured256 pixels and rejects wrong adjacent
quantization/alpha values. Native test images/present unchanged. Source docs:
https://learn.microsoft.com/en-us/windows/win32/direct3d10/d3d10-graphics-programming-guide-resources-data-conversion

After all COM objects/SDL workers destroyed (watchdog still active), standalone
cleanup calls pinned nouveau_horizon_runtime_shutdown(callback) then drains ONLY
tracked successful nvInitialize references. Backend shutdown explicitly leaves
base nv session for process death; NRO return isn't process death, so nvExit is
necessary. Wrapper counts native calls. Query entire former8MB transfer range,
wait up to10s for RW/default attrs after asynchronous service close; otherwise
controlled report instead of unsafe launcher return. Watchdog covers cleanup.
Pinned runtime/header/memory source audited in build/nvk-sdk/source-audit/.
No source/backend semantic fix claimed beyond prototype/harness cleanup.

New build 92915ff5e70a2104c2b4f2abec6658659ae4f183, NRO 22988585 bytes,
SHA256 8c478471176a82e716338e652463e647ad9a151c3c46a07786704fa868789039.
Build, captured pixels ASAN/UBSAN, diff and package SHA PASS; normal NRO unchanged.
Matching ELF/symbols saved. Upload completed, manifest installed=true and
full_readback_verified=true. NRO temp full RETR SHA verified, old917238 backed up,
verified new NRO renamed. Existing standalone env unchanged. Credential scripts
deleted; no active transfers/agents/commits. Next user launches same prototype
through title takeover and opens FTP afterward. Hardware cleanup/float readback
pending; original full game hard freeze remains unresolved.

## EXPANDED TEST CRASH IN TRANSFER-MEMORY ALLOCATION; SMALL STANDALONE PROBE INSTALLED/VERIFIED

User says crashed, FTP ready. Retrieved expanded-result-20261020/ including old
Atmo reports/inventory; NO new Atmo report. No .bin pixel files reached.
Log: C11 ABI checks + audio PASS, crashes during DXVK device init before RGBA8.
Runtime exception257: PC main+144767896, LR main+143574456, FAR0xb6bcf1000.
IMPORTANT offsets are RELATIVE TO MAIN, not image base. Main ELF0xa8330 =>
PC0x8ab7ec8 memset+0x88 (first str q0,[x0]), LR0x89948e8 tmemCreate+0x48.
Thus initial write into returned transfer-memory allocation faults. Allocation
size/caller/permissions unknown. Do not claim specific allocator/ownership cause.
Custom guest runtime exception handler exits via svcExitProcess, preventing normal
Atmo report. Full raw registers absent. First earlier RGBA8 mismatch still unknown.

New EXCLUDE_FROM_ALL target dirt2_dx11_probe_nro links native SDL/OpenAL/DXVK/NVK
plus dxvk_checks/preflight and new dxvk_test_main.cpp, NO guest runtime/COM/FFmpeg.
Custom simple rt::log only; native SDK exception path (no __libnx_exception_handler
in ELF). Logs tmemCreate requested size + caller relative main, and returned
__libnx_aligned_alloc region/permissions/type/attr via svcQueryMemory. Allocation
behavior unchanged. Logs enabled atomically only after log opened. Keeps C11,
audio, both image tests + readback .bin files, blue presentation and watchdog.
Probe ~22MB vs159MB game, also isolates game static constructors/other libraries.
Same existing prototype entry/path; original full DX11 game NRO stays local.
Packaging --standalone-preflight selects small binary, no game assets needed.

Build 917238e822ddc8d605d2a632352a4ca6175de8fa, 22984489 bytes,
SHA256 4ba3b8f215244d08a59e24855e8ae98a0e535c7321df64da83e148cfdd9de866.
Build/ELF exclusion checks/diff/package SHA pass, normal NRO unchanged.
Matching ELF/symbols/ZIP saved. Upload complete: installed=true and
full_readback_verified=true. Env/README/NRO full readback SHA verified, previous
files backed up, NRO renamed last. Credential scripts deleted; no active
transfers/agents/commits. Same DX11 prototype entry now native standalone probe.
User next launches it via title takeover, FTP after exit/error; retrieve logs,
image .bins and newest Atmo report. No backend fix claimed; hardware pending.

## FIRST PREFLIGHT EXITS CLEANLY: AUDIO PASS, RGBA8 PIXEL CHECK FAIL; EXPANDED TEST INSTALLED/VERIFIED

User says returned to homebrew, FTP ready. cfeac run log retrieved to
build/package/switch-dx11/preflight-result-20261020/ (original installed manifest
preserved). Native/Mesa C11 checks PASS. Actual OpenAL context PASS in 0.133s.
DX11 device succeeds. RGBA8 clear/copy/GPU event/Map COMPLETE, but pixel comparison
FAIL at3.127s; exits normally3.136s BEFORE float or Present. No hard hang in this
isolated run. Do not call whole GPU test passed, or infer driver corruption yet:
actual mismatching pixel values were not logged in cfeac and may be rounding.

Expanded preflight logs pitch/first 8 bytes/mismatch count and writes full packed
16x16 readbacks preflight-format-28.bin (RGBA8) and -10.bin (RGBA16F). Continues
float + blue Present after pixel mismatch. Exact expected comparisons unchanged.
Final log boolean includes both readback checks; blue can appear even if a pixel
comparison fails. No backend semantic fix this turn, only evidence collection.

Build 45d542c0120cd988000b05748163c92f5ce84658, NRO 166831913 bytes,
SHA256 f7933167e4bcb305140d530166f72de12d607ad4f375e274b56ba3d3d2a4824f.
Build/diff/package hashes pass, matching ELF/symbols saved. Normal NRO unchanged.
Upload completed: manifest installed=true/full_readback_verified=true. Only
prototype NRO changed; existing preflight env confirmed in retrieved env.txt.
Temp NRO full readback SHA matched, old cfeac backed up and upload renamed.
Credential scripts deleted. No active transfers/agents/commits. User next launches
same DX11 prototype via title takeover, waits for completion or report, opens FTP.
Retrieve .bin files too. Hardware expanded test pending; no backend fix claimed.

## FINAL ISOLATED DX11 PREFLIGHT CFEAC INSTALLED AND FULLY VERIFIED

Final cfeacbe9767fbf5ece1eea8fa9acd4c7f7a60bd8 installed in separate prototype
folder. Env/README/NRO each uploaded to temp, fully retrieved/SHA verified,
previous files backed up and renamed; NRO last. Top manifest installed=true,
full_readback_verified=true; mode isolated graphics preflight. Matching package,
ELF/symbols preserved. Normal game NRO unchanged. Credential script deleted;
no active transfers/agents/commits. User should launch “DiRT 2 DX11 prototype”
through title takeover: native audio/RGBA8+RGBA16F readback/present self-test,
blue briefly on success then homebrew. Reopen FTP afterward. Watchdog direct
abort avoids stdio locks; hardware result PENDING. Game freeze remains unresolved.

## 62A6 HARDWARE C11 CHECKS PASS; FINAL PREFLIGHT READY, FTP LOST

User confirms another freeze and FTP ready. New evidence c11-freeze-20261020/:
C11 native/Mesa TLS/mutex/condition/thread checks PASS on hardware. Thus the ABI
repair is confirmed. Then DXVK init/cache repair succeed; last game log at
21.947s creates float RT 1x1, before audio/first Present. No new crash report.
This is still an unresolved hard freeze, NOT a solved game startup.

Added platform/switch/dxvk_preflight.cpp, opt-in DIRT2_DX11_PREFLIGHT_ONLY=1,
called after SDL init but BEFORE guest memory/image/thread creation. Checks real
OpenAL context, creates DX11 device/swapchain, clears/copies/reads back every pixel
of RGBA8 + RGBA16F 16x16 textures, then clears/presents blue frame for 1.5s,
releases graphics and returns to homebrew. Own preflight shader cache; game
pipeline warmup/record and HUD disabled. Per-stage flushed logs, 10s GPU event
completion limit, separate 30s watchdog -> libnx Timeout user break to obtain
Atmosphere thread stacks. Full kernel/GPU hang may prevent watchdog running.
Failure paths may leave temporary device objects for process teardown; watchdog
includes SDL_Quit. Successful path releases resources. This is DIAGNOSTIC, not
another claimed fix. Hardware result pending.

First preflight cf4a was installed but NOT run; final cfeac has a watchdog
that calls diagAbortWithResult directly, without stdio/filesystem locks. Its tick
comparison also avoids timestamp race underflow. Current final build
cfeacbe9767fbf5ece1eea8fa9acd4c7f7a60bd8, NRO 166831913 bytes,
SHA256 a7e1bebf202ea0829d5470d3425b7540094c726de554bbb5a137aeb50641ada3.
Matching ELF/symbols/ZIP + preflight env verified. Build and diff check pass.
Final upload failed BEFORE connection/login: first timeout, retry No route to
host at 192.168.1.148:5000. No bytes from final cfeac uploaded. Async question
asks user to reopen FTP/keep awake or give changed address. User reply pending.
IMPORTANT: first diagnostic cf4a DID finish installing and full readback verify;
its manifest saved at c11-freeze-20261020/first-preflight-installed-manifest.json.
Thus console currently has cf4a + PREFLIGHT env (not the game); local top manifest
is final cfeac installed=false. Do not claim cfeac installed. It removes watchdog
stdio/logging so a log-lock stall cannot block report request, and guards tick
race underflow. Install cfeac when FTP returns, env/README/NRO + readback hashes,
back up cf4a NRO, NRO last. Normal game NRO unchanged.
Credential scripts deleted; no active transfers/agents/commits.
Once final cfeac installed, user should launch SAME separate DX11 prototype via
title takeover: short isolated audio/GPU test, blue frame on success, returns to
homebrew. Reopen FTP after result. A full kernel hang can defeat watchdog.

## LATEST FTP CHECK: NO NEW RUN LOGS; USER RESULT CLARIFICATION PENDING

User only said “FTP ready” after 62a6 installation. Retrieved current logs and
crash inventory to build/package/switch-dx11/c11-result-20261020/.
Prototype dirt2.log is BYTE IDENTICAL to c2e5 freeze log (SHA256
2a4ae7575b775236abacb415aeb5b616f2f1dd4ca8cd971622c66e4d0c968db8),
modified 15:53:04 console time, before NRO upload 16:04:28. DXVK log also old;
no C11 ABI self-check lines. No new Atmosphere crash report. Normal game log
also predates the new build (14:54:40). Cannot infer whether user hasn't run it
or latest build failed before logging. Asked async whether latest prototype was
launched and what happened. Do not claim C11 repair failed or passed on hardware.
62a6 remains installed/verified. No new changes/install this turn.
Credential fetch scripts deleted; no active transfers/agents/commits.

## NVK PROTOTYPE: C11 ABI COLLISION REPAIR INSTALLED/VERIFIED; TLS EXHAUSTION DIAGNOSIS RETRACTED

Latest user: c2e5fbbc froze, forced restart, FTP ready. Retrieved logs and LUT to
build/package/switch-dx11/freeze-cachefix-20261020/. No new Atmosphere crash.
Cache repair DID work: invalid LUT rejected/recreated; first Present reached at
43.110s. Audio still threw al::tss::tss(); logs stop during multi-line swapchain
properties output, before any completed Present. Full system freeze cause remains
unconfirmed; do not claim ABI repair has already resolved it.

DEFINITE discovery: OpenAL compiled against native C11 thrd_success=4, while
Mesa's bundled threads_posix.c.o exported 25 same-named functions returning
thrd_success=0. Thus successful Mesa tss_create was interpreted as OpenAL failure.
Earlier TLS exhaustion inference was WRONG. Removed platform/switch/tls_keys.cpp,
tests/switch/test_tls_keys.cpp and CMake TLS wrappers entirely. Native 12-key limit
exists but was not demonstrated to be exhausted.

New tools/scripts/prepare_switch_nvk.py creates build/nvk-sdk/libvulkan_dirt2.a
with all Mesa C11 definitions AND references renamed dirt2_mesa_*, plus private
__xpg_strerror_r stub. Original extracted SDK untouched. Script verifies all 26.
Also SDL imported interface requested -lvulkan and initially silently reintroduced
original archive! Fixed by aliasing its vulkan target to switch_nvk. FINAL ELF
verified: native tss_create success=4, Mesa success=0, no -lvulkan fallback, no TLS
wrappers. Other strong libc symbol overlaps were checked (only those 26).
Prototype-only dxvk_checks.cpp now verifies both TLS ABIs plus native mutex,
condition and thread creation/join before SDL/GPU; fails cleanly before game if
checks fail. Patch 0003 adds vkCreateSwapchainKHR enter/result stderr logs.
Build, archive-symbol checks, patch preparation, package hash, normal NRO unchanged,
Python/shell syntax and git diff --check pass. Hardware checks PENDING.

Build 62a6ad17ef2c742fa596967d5c9edeac4cf9573f, NRO 166819625 bytes,
SHA256 1fee2f80490154a7bacedfb393e6c2e3c4e1c894f015bf680781ad09c09d4247.
Matching ELF/symbols/ZIP saved. Upload finished: installed=true and
full_readback_verified=true in manifest, temp NRO fully retrieved/hash checked,
previous c2e5 backed up and verified temp renamed. Normal deko build unchanged.
Credential scripts deleted. No active transfers, agents or commits.
User should launch separate DX11 prototype through title takeover and report
result, then reopen FTP. Hardware ABI self-check/audio/presentation pending.

## DX11 CRASH IDENTIFIED: TORN CACHE ALLOCATION, REPAIR INSTALLED AND VERIFIED

Latest crash 01792503619_03db12780bd84000.log matches 789128ad. The initiating
thread is in std::bad_alloc/abort from DxvkShaderCache::readShaderLutKey while
parsing the cache left after the first forced shutdown. The reported user break
2345-0008 in hidGetTouchScreenStates is SECONDARY: main thread polls SDL while
aborting worker has already run libnx cleanup. Do NOT fix this by bypassing HID.
Evidence/logs/cache/report/JPG saved in build/package/switch-dx11/crash-tls-20261020/.
LUT has 728 valid entries then an invalid XFB count 1201970788; unchecked resize
requests tens of GB. Audio TLS repair not yet hardware-validated (fails earlier).

New separate Switch DXVK patch 0002 bounds cache reads and XFB counts before
allocation, validates binary entry extents, catches parse exceptions and clears
partial lookup entries before rebuilding the files. Also fixes StlFile's missing
m_flags initialization (size() must distinguish write-only). Apple DXVK untouched.
prepare_switch_dxvk.py now handles ordered patch files. Captured-cache regression
compiles actual patched readShaderLutKey/readShaderXfbInfo + header helpers and
util_file.cpp, passes ASAN/UBSAN (728 valid entries then safe rejection), checks
overflow and write-only size/append. Native Switch DXVK/game builds, patch apply,
and git diff --check pass. TLS multiplexing retained.

New build c2e5fbbc92958e010570d9f467ea9859339041bb, NRO 166819625 bytes,
SHA256 4a523d88404d102cb037a97a3cf113b4309bfa259125a33684f1cd813fde9ea4.
Matching ELF/symbols/package preserved. Upload complete: manifest installed=true,
full_readback_verified=true. Full temporary NRO readback matched SHA256, previous
789128ad backed up, verified upload renamed. Normal deko NRO SHA unchanged.
Prototype ZIP matches rebuilt NRO. Console malformed cache deliberately retained
to test rejection/rebuild. User should launch separate DX11 prototype through
title takeover once more, then open FTP after result. Hardware result pending.
Credential scripts deleted; no active transfers, agents or commits.

## DX11 FIRST-RUN HANG: TLS ALLOCATION REPAIR INSTALLED AND VERIFIED

Retrieved prototype logs after user forced restart (POWER 20 seconds). Evidence:
build/package/switch-dx11/hang-20261020/ (includes original installed manifest).
No new Atmosphere crash report: latest report predates prototype. Logs show NVK
GM20B initialized, feature level 11.0 device/swapchain creation succeeded, then
OpenAL al::tss::tss() allocation exceptions around 28 seconds; no completed first
Present. Cannot yet attribute the full system hang solely to this error.

SDK disassembly confirms threadTlsAlloc only accepts 12 slots. New prototype-only
platform/switch/tls_keys.cpp multiplexes all newlib pthread/C11 TLS syscalls through
one native libnx slot with 256 logical keys, generations preventing stale reuse,
per-thread storage and four reentrant destructor passes. Native libnx service TLS
unchanged. tests/switch/test_tls_keys.cpp ASAN/UBSAN PASS for exhaustion, isolation,
reuse and callbacks. Linked disassembly confirms all four pthread TLS operations
branch to wrappers. Added audio-context result + entry-to-first-Present logs.
Build 789128ada90328e4e69dca0b18004342ce9ebe8d, NRO SHA256
aeceb951bf4fdb9367f3ee4fdda2383839c1d031d244ec7ded069977cc763765,
166815529 bytes. Matching ELF/symbols/ZIP preserved. Component build and diff check
PASS. Hardware result pending. Normal deko build unchanged.

Upload completed: installed=true/full_readback_verified=true in manifest.
Separate prototype NRO fully readback/hash verified, old 6f448 NRO backed up,
verified temp renamed. User should now test the separate DX11 prototype once and
report whether it reaches the intro/menu, then reopen FTP. Full-hang resolution
still unconfirmed. Credential scripts deleted; no active transfers/agents/commits.

## DX11 → NATIVE DXVK → NVK PROTOTYPE INSTALLED AND VERIFIED (2026-10-20)

User requested a DXVK DX11 → NVK prototype and explicitly confirmed FTP ready.
Separate “DiRT 2 DX11 prototype” installed at /sdmc:/switch/dirt2-dx11;
NRO and package files fully retrieved and SHA256 verified. Manifest:
build/package/switch-dx11/manifest.json: installed=true, full_readback_verified=true.
Build ID 6f4484f0b15cc68c158bcf4b002dca9d318cdcb0, NRO 166815529 bytes,
SHA256 8f4c6aaa4e2b329134005a17b22717bb4c07a3dffeb1d18069e7ea7449dc2a62.
Matching ELF/symbols and ZIP saved in build/package/switch-dx11/.
Normal e594 deko executable, shader pack and environment preserved unchanged.
Prototype shares only assets at sdmc:/switch/dirt2/Dirt 2; its settings/saves/logs/
Mesa cache live in sdmc:/switch/dirt2-dx11. Env selects dx11, SDL3 Vulkan WSI,
info logging and fps/compiler HUD. No startup conformance tests or SYNC enabled.

Native DXVK D3D11/DXGI and NVK statically linked, no Wine/FEX/deko/EGL.
Isolated pinned switch-dev SDK, patched separate DXVK tree, Vulkan-only SDL;
system SDK and normal installed SDL unchanged. Source/provenance/build procedure:
docs/SWITCH_DX11_PROTOTYPE.md and tools/scripts/build_switch_dx11.sh.
Component builds, link/symbol checks, ZIP/hash checks, patch reproduction and
UTF-16 ASAN/UBSAN test passed. Full convenience script not run end-to-end;
equivalent component commands succeeded. Hardware launch/rendering/performance
remain UNTESTED: user should launch this separate entry through title takeover,
allow initial shader compilation, test menu/race, then reopen FTP for logs.
Normal e594 diagnostic visual result is also still pending.
Credential installer deleted. No active transfers, agents or commits.

## E594 STATE-RESTORATION DIAGNOSTIC INSTALLED AND FULLY VERIFIED

User supplied five Switch recordings and confirmed the visible green menu flashes,
blue billboard and purple race/collision frames are the intended issues. Downloaded
all five from emuMMC/SD00/Nintendo/Album/2026/10/20, with size/hash verification.
Evidence: build/package/switch/video-evidence-20261020/, analysis.md,
temporal-metrics.json, original MP4s, contact sheets and selected PNG frames.
No new GPU dumps since f3844. Latest log has no skipped draws/native GPU error.
Purple scene retains geometry/HUD; do NOT claim a normal buffer was presented
based only on its appearance. Cause still UNCONFIRMED. Healthy prior f7268 LUT
matches CPU math within 0.000512 mean error; constants newly retrieved for draws
288/289 and f7654 270/271. This is not validation of the failing video frames.

New build e594831c05d28a4de50747c9a76af266071c5c6e, NRO SHA256
698db79c66ebf324df3d52b9da21e9357f867f420791e52d0f8e2459f020a7f0,
151111465 bytes. Opt-in DIRT2_DEKO_REBIND_STATE=1 restores every draw's native
state without per-operation CPU waits or additional explicit target publication
barriers for unchanged targets. Normal checkpoints/fences preserved. New
flush_targets(bool publish=true) defaults to original behavior. Default normal
package has diagnostic OFF; installed environment will enable PERF + REBIND only.
SYNC/tests OFF, ZCull ON. Full-lighting barrier pack ae8fb7... unchanged.
This is an ISOLATION EXPERIMENT, NOT a confirmed fix. Extra binding commands can
still alter timing. If clean, inspect missing state invalidation/binding-sensitive
behavior; if still broken, investigate shader/data-dependent passes/dependencies.

Switch build, diff check, render_hazards (including no-extra-barrier test),
async_command_batches and alpha_target_transitions ASAN/UBSAN PASS. Normal ZIP
and sd layout match e594 + confirmed barrier pack; normal env comment only.
ELF/symbols saved under symbols/e594... and video-state-probe-20261020/.
Upload completed: manifest video-state-probe-20261020/manifest.json has installed
and full_readback_verified TRUE. Both env and NRO fully retrieved/hash verified
before unique backup+rename; NRO last. Full-lighting barrier pack unchanged and
verified. Installed env PERF + REBIND only; packaged normal env disables diagnostics.
User should now repeat menu navigation and a short race/collision, preferably
recording any remaining flashes, then reopen FTP. Hardware visual outcome PENDING.
Credential scripts deleted. No active transfers, agents or commits. No source fix
for the live video failures has been proven.

## FC30 INSTALLED AND FULL READBACK VERIFIED — READY FOR USER

Installedfc30cab7623d1d5fceb390db769f49c419da0ab7, SHA256
c7b1481bdd4e7fd38bc53b254ffa5410570100932dbc8e2466bdbea173f7b1ab.
Manifestglobal-dxvk-audit-20261020/manifest.json installedTRUE,
full_readback_verifiedTRUE; atomicallybackedupold3ee6NROandrenamedverifiedupload.
ONLYNROchanged. Full-lightingbarrierpackae8fb7...andenvPERFonlyunchangedverified.
StartupTESTSandSYNCremainOFF,ZCullON. PackageZIP/sd/symbolsmatchingfc30verified.
UserconfirmedracebarriersFIXED on3ee6 andgameinitworking. GlobalAPIthreefixs
VS/PSGetFunctionbounds/UpdateSurfaceorigin/readbackcompatibilitynowinstalled;
newfc30game/hardwareeffectstillawaitinguser. Logs/prev/env/captureinventory
preservedunderbefore-install-<stamp>. Nonewdumpssincef3844. Noactivecommands,
networktransfers,agentsorcommits. Temporarycredentialscriptdeleted.

## FC30 CORRECTED BUILD UPLOADING — USER CONFIRMS FULL-LIGHTING BARRIERS FIXED

User explicitlyrequestedinstall andFTPready; confirmsracebarriersFIXED on3ee6.
Thusnormal3ee6startup succeeded afterstartupTESTflagremoval andfull-lighting
ed355rewrite hasgamehardwareconfirmation. Extended-wrappervariantnotseparately
visuallyvalidated; originalpackcandidateisconfirmed. Loglate-run~29.5–30.5fps,
287draws/frame; donotclaimcontrolledspeedupversusdifferentpriorscene.

Uploaderexecsession28176 /tmp/dirt2-install-audit.py currentlyrunning.
NROfc30cab7623d1d5fceb390db769f49c419da0ab7 withthreeglobalauditAPIcorrections.
Manifestglobal-dxvk-audit-20261020/manifest.json installedFALSEuntilfullRETRhash
andatomicbackup/renamecomplete. Logs/prev/env/captureinventorysavedto
before-install-<stamp> underthatauditfolder. Remoteallcapturesunchangedpreserved.
Validatedremotefull-lightingshaderpackae8fb7...unchanged; envexactlyPERFonly.
UploaderchangesONLYNRO. NoSYNC/testflag/noothernewdebugoptions. ZCullon.
MainZIP/sdpackage/symbolsnowfc30;packageenvcommentonly. NeedwaitinstalledTRUE
beforeaskinguserlaunch. Credentialscriptmustdeleteaftersuccess. Priorheaderbelow
uninstalledstatusstaleoncefinished. Noagents/commits.

## GLOBAL DXVK AUDIT COMPLETED — THREE NEW LOCAL FIXES, NO DEPLOYMENT

User requested broader comparison beyondknownissues. ReadlocalDXVKapi/resource/
shader/constant/query/presentation/statecodeandours. Engineeringreport
 docs/D3D9_DXVK_AUDIT.md has18prioritizedrows,source-scope/limits,matchedareas,
false-positivechecksandcoverageorder. LinkedfromD3D9_TESTS.md. APIinventoryJSON
build/package/switch/global-dxvk-audit-20261020/api-coverage.json:119Deviceentries
inclIUnknown,16inheritedstubs;17D3D9entries,2stubs. Notbehaviorcoveragepercent.
DXVKreferencecc92063b972e28cae662fdd9032fb15179179b5e treeonlyuntrackedstamp.

ThreeconfirmedbugsfixedLOCALLY(notuploaded):
1) VertexShader/PixelShader GetFunction boundedbytecopy min(callerCapacity,bytes),
queryupdatesrequiredsize; copydoesnotupdatesize, matchingDXVK. Oldcodeoverran.
2) UpdateSurface nullpDestPointnowdefaults0,0, notsourceRect.left/top.
3) GetRenderTargetData rejectsformat/dimensionsmismatchBEFOREbarrier/DMA, rather
thanmincropandpotentiallyincompatiblepitch. Otherbroadervalidationstillpending.
Threeactual-methodASAN/UBSANtestsPASS: test_shader_get_function.py,
test_update_surface_origin.py,test_readback_dimensions.py. SwitchNRO builds,
diffcheckPASS. MatchingcandidateNRO/ELF/symbols+manifest savedin
build/package/switch/global-dxvk-audit-20261020/. BuildID/hashinmanifest.
CURRENTbuild/switchNRO/ELFarethisNEWUNINSTALLEDCANDIDATE. Installedconsole+mainZIP
remain3ee6; symbols3ee6previousmatch. Noshader/env/FTPchanges thisturn.

Largergapsconfirmed: permissivecapabilityqueries, dirtyregion/mipmatching,
SYSTEMMEM/SCRATCHGPUimages, present-onlyretirement, descriptoroverflowreturns0,
offline-onlyshaderfallback, sRGBfallbackbeforeblend, limitedqueries, incomplete
swapchainrouting/Presentrects/intervalhandling, Reset/lostdevice/softwareVP/gamma.
Reportdistinguishesplatformlimitsfromactualgamecausality; noassertionthese
explainremainingflicker. EvictManagedResources/ValidateDevice alsoDXVKno-oplike;
GetDeclarationignoresinputcountONPURPOSE/native/Wine, NOTlikeGetFunctionbug.
Constantpushreuse NOTprovenhazard solelybecauseDXVKusesfreshUBOslices.
Noagents/commits. Usercanstillbetestinginstalled3ee6. NextFTPfetchfirst,avoid
replacingtestbuildwithoutcollectingresults/captures. Full-lightingbarriersand
normalgameinitialization3ee6hardwareoutcomestillpending.

## 3EE6 NORMAL GAME BUILD INSTALLED — STARTUP TESTS REMOVED

Installed3ee6bcf83f779658d3dbfb199247c2cf8cba6483. AllstagedbytesfullRETRhash
verified beforebackup/rename,NROlast. Manifestdxvk-state-candidate/manifest.json
installedTRUE. EnvexactlyDIRT2_DEKO_LOG_PERF=1 newline; noSTARTUPTESTS,noSYNC.
ZCullon. Full-lightingpackae8fb7...unchanged; baselineed355base-onlyprobeNOTinstalled.
NormalZIPandsdpackage/symbolsnowmatching3ee6. Noactivetools/transfers.

Nextuserlaunch MUSTestablish whether removingteststartup restoresgameinit and
whetherfull-lightingbarriersclean. Bothunverified because685testbuildnevermade
firstPresent; userclarifiedthisAFTER46testpassretrieval. 46testassertionsandimage
comparisonstillvalid but NOTproofofgamesuccess. Liveglitchesunresolvedpending
newlive.bmp/screen.bmp. LatestAtmostackdecoded: SDLeventthreaduserbreak in
hidGetTouchScreenStates/PollTouch/PumpEvents/SDLWait/main, NOTallocationstack.
AppLogisallocationfailureevidence; donotblameHIDfromthatstack.

Credentialscriptsdeleted. Noagents/commits. Priorinprogressheadercompleted.

## CURRENT FTP TURN — TESTS PASS BUT 685 GAME STARTUP FAILED; NEW NORMAL BUILD UPLOADING

FTP succeeded. Retrieved batched-hardware-20261020/{dirt2.log,prev,env.txt,tests/*}.
All46SwitchtestsPASS and all46imagesmatchMacwithincomparator tolerance:
comparison/comparison.json 0failures. No NEW GPUdumps afterf3844. DoNOTinfergame
launched. User says previousrunFAILEDinitialization, so full-lightingbarriers
NOT yet hardwaretested (base-onlyclean waslastknownconfirmed).
Log actualfailure: testscomplete2.756s, gameCreateDevice2.760s, fatal
'dkMemBlockCreate: initialization failure (result1)' at19.143s BEFOREfirstPresent.
RetrievedAtmospherecrash_reports/01792495292_03db12780bd84000.log MATCHES
685F68C39D93EA5542D7D67DCE8385016B2FDC38. Not previousGPFIFOtimeoutclass.
OffscreensuitehasnoPresent; defercallbacks+frame-transientmemoryremainuntil
frame retirement, so retainedtestresourcesplausiblyincreasegame-startupmemory.
Not exactallocation-rootproof. DoNOTenable game-startup testflagagainuntil
explicitresource-retirement/lifecyclefixorstandalone runner. Removalflag current
normalgame repair restoresnon-testpath; stillneedsrealuserlaunchvalidation.

INSTALL IN PROGRESS execsession42236 /tmp/dirt2-install-dxvk.py.
Prepared3ee6bcf83f779658d3dbfb199247c2cf8cba6483 NRO with DXVKstatefixes,
full-lightingshaderpackverifiedunchanged ae8fb7..., pendinghardwarevalidation.
Env ONLYPERF afterremovingDIRT2_D3D9_TESTS (startup46resultsalreadyretrieved).
Envstaged/fullreadbackverified; NROupload/readbackpending. Manifest
build/package/switch/dxvk-state-candidate/manifest.json installedFALSEuntildone.
Full stagingbeforeanyrename,NROlast,backupsretained. PackageZIP/sd now3ee6,
symbols/3ee6...elf saved,packageenvcommentonly. NeedwaitinstalledTRUEbefore
userlaunch. Currentconsole685untilfinalrenamephase. NoSYNC,ZCullon.

Recent3Atmoreportsalsofetched; latestmatching685 isabove. Addr2lineexecsession95386
running todecode stack frommatching685ELF. Othersolderandnotcurrent.
OnceinstalledTRUE, cleanupcredentialstempfiles /tmp/dirt2-ftp-review.py,
/tmp/dirt2-install-dxvk.py,/tmp/dirt2-fetch-crash.py. Askuserlaunchnewnormalbuild,
confirmbarriersfull-lighting,and -/+ captures(ifcanlaunch),thenFTP.
Noagents/commits. Docs46hardwarepass+subsequentstartupallocationfailureupdated.

## DXVK AUDIT — TWO LOCAL FIXES PREPARED, INSTALLED TEST BUILD UNCHANGED

User requested consulting DXVK. Read working local DXVK D3D9 source at
build/src/dxvk/src/d3d9/d3d9_device.cpp, revisioncc92063b972e28cae662fdd9032fb15179179b5e.
Concrete gaps fixed in source (NOT uploaded; user testing685 build):
1) active_texture_mask from selectedPS/VS metadata (pixel0..15,vertex16..19).
Mip generation onlyforactive samplers. prepare_feedback(sampled_mask) snapshots
only sampled attachments, clearsunusedpreviousfeedbackbindings. Previouslyall
boundtextures couldcause unnecessarywholeimagecopies/barriers eachdraw.
2) SetRenderTarget now invalidateskBlend, sinceRT0samplecountaffectsATOC/native
alphatest. ADAPTIVETESS_Y nowinvalidateskBlend aswellas raster. Previouslynative
alphatestcouldremainAlwaysafterATOCturnedoff orMSAAtargetreplacedbysingle.

Tests: test_gpu_state_paths.py updatedactualmask/feedbackcode andASAN/UBSANPASS;
newtest_alpha_target_transitions.py usesactualSetRT/SetRS/alpha-state emission,
100repeatedMSAA/singleRTtransitions and100ATOCtoggleswithoutperdrawflush PASS.
Negativechecksrestoreeacholdinvalidationpathseparately; bothfailasexpected.
Existingalpha-packcompatibility andrender-hazardtestsPASSASAN/UBSAN.
SwitchNRO builds. Candidate artifacts+manifest in
build/package/switch/dxvk-state-candidate/ (buildID/hash inmanifest).
Current build/switch/dirt2.nro/elf are NEWUNINSTALLEDCANDIDATE; installedmatching685
NROremainsinbuild/package/switch/sd/switch/dirt2 andsymbols/685...elf.
MainZIPstill685. NoFTP/networkmutationsthisauditturn; noagents/commits.
SourcefixesnotprovenfixlivegameflickerorFPS; quantifywithhardwareaftercurrentrun.
Docs/D3D9_TESTS.md includesDXVKcomparisonandlimitations. DoNOTblindlyreplace
PushConstantswithDXVKUBOsorcopyVulkanbarriers; differentnativepathsneedvalidation.

Next: fetchuser685startup46testresults andlivecaptureswhenFTPready,remove
DIRT2_D3D9_TESTS afterretrieval; inspectfull-lightingbarrierhardwareoutcome.
Then installpreparedDXVKstatecandidateasappropriate. NoSHADERSwerechangedthis
turn; full-lightingcandidatefrompreviousremains. Allpendinghardwarechecksbelow.

## INSTALLED AND FULL READBACK VERIFIED — READY FOR USER TEST

Build685f68c39d93ea5542d7d67dce8385016b2fdc38 is NOW installed successfully.
Journal deployment-barrier-batched.json installedTRUE: all3files SHA256verified
by fullFTPreadback beforebackup/rename,NROlast. Noactive tools/transfers.
Full-lighting shader candidate REPLACES temporary base-only diagnostic. User must
confirmbarriersstillclean WITHlightingrestored; hardware result PENDING.

Startup now runs46offscreenregressiontests automatically thencontinues into game.
Mac46/46pass. Switchnew5casesPENDING. Results willbe
sdmc:/switch/dirt2/d3d9-tests/batched-685f68c3/results.json plusimages.
Remove DIRT2_D3D9_TESTS envline AFTER retrievingresults nextFTP turn (temporary).
OnlyotherflagPERF;SYNCoff;ZCullon. NormalZIPcomment-onlyenv.
Manual -/+ now saves live.bmp BEFOREdetailedcapture's per-drawreadbacks.
User'spreviousscreensdidnotshowliveissue; comparelive.bmp vs screen.bmp nexttime.
No claimflickerfixed; no neworder/statefixwithoutrepro. Nextaskuserlaunch,check
barriers,andcaptureliveglitches,thenFTP. Prioruploadheader below nowcompleted.
DeletedtemporaryFTPcredentialsscripts. Noagents/commits. Localchecksbelow.

## CURRENT TURN — FULL-LIGHTING BARRIER CANDIDATE UPLOADING + BETTER CAPTURES/TESTS

User confirms barriers clean with base-texture-only shader. Thus diffuse sampling
works in that probe; full lighting/native shader path remains suspect (not exact
compiler root proof). New dumps retrieved successfully via FTP:
barrier-probe-evidence-20261020/20261020-105805-f2938-0 and
20261020-110022-f3844-0, all root targets/screen/logs, selected geometry/constants
and texture mip chains (24 draws/18 texture IDs each). User says screenshots did
NOT capture issues seen live. Detailed captures serialize/read back each draw,
and rebind full state each list, so can mask timing OR stale-state failures.

New bridge capture saves live.bmp of the just-rendered ordinary frame BEFORE
next frame's detailed readbacks; main screen.bmp remains detailed frame. Metadata
records frame numbers. One final readback waits after normal frame draws, no
intermediate captures in that frame. This is not historical video; brief flicker
can still be missed. NRO built successfully:
685f68c39d93ea5542d7d67dce8385016b2fdc38,151111465bytes,
SHA25627921ef83039efda8ca91a2d6fc193f6d2b1a07077716f52be6aadc9db03a3e3.
Symbols saved matching build. Package uses full-lighting single-shader candidate.

Full lighting implementation in tools/dx9shader/workarounds/barrier_lighting.glsl,
census fix_barrier_lighting scoped exact SHA1 and old/new GLSL source hashes.
Preserves shadow-compare wrapper and alpha/fog epilogue for new metadata. Both
variants compile. 20000 CPU algebra evaluations matched original within5.4e-7.
Pack build/shaders/barrier-full/dirt2.dkshpack SHA256
 ae8fb7d91f84b359752c1b310e49219665531ea7b26ae0b07f4d82980f3c87a6
contains all897 baseline entries, only ed355 shader binary differs, allmetadata
identical. Original build/shaders/dx9 pack remains untouched. Hardware outcome
PENDING, do not call full-lighting fixed until user tests.

User requested more test coverage. Added5 original offscreen batched tests to
conformance.cpp (constants,alpha,LOD,resource retirement,RT-to-texture),64tile
iterations, one final readback, no intermediate reads. All46 cases PASS on Mac
DXVK in build/d3d9-tests/dxvk-batched-20261020; Switch newcases NOT yet run.
These reuse builtin shaders; do NOT reproduce full barrier arithmetic. Audit
latest hardware Wine report actually141pass/2fail/1partial of144Macpasses:
depth_blit_test,srgbwrite_format_test fail; surface_blocks partialUYVY.
Many relevant alpha/blend/MRT/constants/attachedRT tests alreadypass. AuditJSON
barrier-analysis-20261020/test-coverage-audit.json. Docs updated honestly.
ASAN/UBSAN actual asyncbatch + alpha compatibility tests PASS; diffcheckPASS.

INSTALL CURRENTLY RUNNING: exec session91901, script/tmp/dirt2-install-current.py,
journal build/package/switch/deployment-barrier-batched.json installedFALSE until
complete. Full pack andenv staged and readbackhashverified; NRO uploading or
verifying. Allthree staged before anyrename; NRO renamedlast. Full backupnames
in journal. Console stays previousdiagnostic until finalrenamephase.
New consoleenv deliberately adds DIRT2_D3D9_TESTS=sdmc:/switch/dirt2/d3d9-tests/batched-685f68c3
plusPERF only. Tests run atstartup, then game continues. Remove testflag after
fetching result nextturn; SYNC remainsOFF,ZCullON. Normal ZIPenvcommentonly.
After installedTRUE ask user launch (tests automatic) inspectbarrierlighting,
take -/+ dump when liveglitch visible, then reopenFTP. Do not reinstall suiteNRO.
No permanent flicker fix this turn; newcapture will help isolate it.
No agents/commits/unrelatedfileedits. TemporaryFTPcredentialscriptsmustdelete
when installercomplete: /tmp/dirt2-ftp-current.py,/tmp/dirt2-fetch-details.py,
/tmp/dirt2-install-current.py. Prior history below may be stale.

## LATEST TURN — FTP REFUSED, NEW CAPTURES NOT YET RETRIEVED

User says dumps and FTP ready. Two scoped WiFi/en0 attempts to192.168.1.148:5000
returned ConnectionRefusedError (not timeout). en0 still192.168.1.94/24 active.
Async question asks user reopen FTP/confirm current IP. No new downloads or
remote mutations this turn. Console STILL temporary base-only barrier shader,
NRO973. Need restore full shader after diagnostic; no permanent lighting removal.

Prepared readable, mathematically equivalent full-lighting PS candidate offline:
barrier-analysis-20261020/readable.frag.glsl +.dksh/.tgsi. CPU algebra checked
against original translated shader for20000 randomized finite cases: max absolute
error5.36e-7, mean5.23e-9. readable-validation.json saved. NOT hardware-validated,
NOT installed, NOT integrated into permanent shader generation. This preserves
lighting/shadows/fog/material/specular/tone mapping; only expresses registers as
named scalar/vector values. Use only if base diagnostic confirms clean textures.
CPU reference/results copied from /tmp into same evidence directory.

Important synchronization distinction: SYNC flush creates new list everydraw,
so renderer also restores ALL state eachdraw. Cleaner SYNC output does not by
itself prove resource-ordering hazard; stale state tracking is another candidate.
No new renderer source changes in this turn. Prior batching and alpha tests not
rerun because source unchanged. No builds/install/commits/agents this turn.

## CURRENT INVESTIGATION — BARRIER DIAGNOSTIC INSTALLED, ASYNC FLICKER UNRESOLVED

Current NRO remains 97308ab4459d5fe413ba343a7082d9868d370361, successful launch.
User reports substantially more spontaneous flickering without forced per-draw
SYNC. Do not treat asynchronous correctness or performance as solved.
ZCull enabled; normal environment contains only DIRT2_DEKO_LOG_PERF=1.

TEMPORARY single-shader diagnostic is installed on console: base-texture-only
PS ed3554e760bf in original shader pack. Manifest and pack in
build/package/switch/barrier-analysis-20261020/base-only-deployment.json and
base-only.dkshpack. All other shaders/metadata unchanged; NRO/env unchanged.
MUST RESTORE full baseline pack after diagnostic; removing lighting is not a fix.
User now says new dumps/FTP ready. First retrieval attempt refused connection at
192.168.1.148:5000; asked asynchronously to reopen FTP/confirm address.

Collected old barrier geometry/constants/all sampled mips for f7268/f7654 plus
new async capture 20261020-104105-f10819-0 in async-render-evidence-20261020.
Sampler2 is pink screen-lighting mask; sampler3 white car-shaped shadow mask,
not main color framebuffer. Managed texture dump reads CPU storage, so intact
base assets do not establish actual GPU sampling correctness. Projection q=1
rewrite compiles byte-identical DKSH, so not a useful candidate explanation.
CPU geometry+GLSL shader reference /tmp/dirt2-barrier-reference.py reproduces
barrier positions and clean dark advertisements, while native left barriers are
bright/incorrect. Comparison includes occlusion/anisotropic differences, so this
is NOT proof of a compiler defect. Results /tmp/dirt2-barriers-*.png/.npz.
Await diagnostic screenshot: clean base logos suggests lighting/native shader;
still screen/car suggests GPU texture sampling/mips/descriptors.

Synchronization audit ongoing, no new source fix yet. Buffer locks rename or
wait; transient/staging and deferred frees use frame fences. Readback already
invalidates CPU cache. SDK PushConstants intentionally uses constant-load path;
persistent UBO reuse alone is not established unsafe. Descriptor exhaustion
silently returns slot0 but no exhaustion messages in logs. Do not assert causes
without evidence. Continue image write/read ordering and cache audit.
No commits or agents. Prior history below is chronological and may be stale.

## CURRENT BUILD LAUNCHES — BARRIER SCREEN-COPY ISSUE STILL PRESENT

User confirms97308ab... starts and so far looks good. Thus async completed-op
batching has passed at least one hardware launch without forced per-draw SYNC.
Do not infer measuredFPS/allalpha fixed yet. User asks investigate earlier race
side barriers showing main screen/back of car; async followup confirms SAME
barrier issue persists in current build. Asked user to reopenFTP when finished
and optionally captureclose barrier. FTPcurrently notknownactive (game running);
no FTPattempts or deployments this turn. No sourceimplementation edits.

Offline analysis saved build/package/switch/barrier-analysis-20261020/analysis.json
and draw-records.json. Parsed all draw/shader/sampler logs from root copies of
alpha-evidence-20261020 f7268/f7654 races. Candidate roadside material shader
PS ed3554e760bf / SHA1d5ba8ef2993994824ce136a7ad5348067cab3869, VS18140899efed
/SHA134f8ed91e44d32f9e4e2a4e25fe2d20ea5903c6c (also VS3d92452474d0).
Four2Dsamplers: s0 DXT1advertisement1024x256, s1 material512x128, s2 fullsize
960x544fmt21 screenlighting, s3 halfsize480x272fmt21 shadow/mask. No cubemap in
this PS. PIAA/FLY source images intact in oldmanual-evidence-2/f15233/textures.
Do NOT assume corrupted baseasset or disable legitimate screen-space sampling.
Lighting-texture binding/storage vs shaderprojection/arithmetic remainsunknown.
Oldlocalrecords do NOTinclude selectednewtexturemips or rawgeometry; must fetch.

Next FTP step: read analysis.json downloads plan (candidate drawIDs + textureIDs
for 20261020-100224-f7268-0 and20261020-100512-f7654-0). Fetch selected geometry
.drawtxt +referencedrawVB/IBfiles and matchingconstants; textureIDs allmips/layers.
Fetchlog/envforFPS/current973 too. Preserve installed973 andoldshaderpack while
analyzing; user testing currentbuild. Then inspect actualsampled screen images,
mips and geometrically identify barrier draws; CPUshader reference ifneeded.
Tempanalytical GLSLcopies /tmp/dirt2-ed3554e760bf.glsl and VS18140899efed.glsl;
/tmp/dirt2-race-draws.json alsohasparsedrecords. No active tools/agents/commits.

## ASYNC BATCHING BUILD INSTALLED AND VERIFIED — AWAITING LAUNCH

2026-10-20 candidate 97308ab4459d5fe413ba343a7082d9868d370361 installed.
Full NRO readback SHA256404b0a30c2ad84dc4422abb3a1961d17bea28e4b058e2e2d74578c0a770d0817
verified before atomic backup/rename and SIZEcheck. Journal
build/package/switch/deployment-async-batches.json installedTRUE with backup name.
Original working897shaderpack/FFPfiles unchanged and verified. Environment
preserved exactly: ONLY DIRT2_DEKO_LOG_PERF=1, no SYNC/SELFTEST/LOG_RESOURCES.
NRO retains alpha compatibility repair; adds completed-operation async batches
of16, removes ineffective native threshold override; preserves native error logs.
Symbols and package current. No tools running. Deleted temporary FTP scripts.

Asked user async to launch; if successful inspect menu masking/leaves and drive
briefly for FPS, reopenFTP. Hardware launch/visual/FPS result still PENDING,
not established by CPU model tests. If same nativeerror, retrieve currentlog and
Atmosphere; new failure may occur at first asyncbatch BEFOREfirstPresent now,
which narrows failure. Root beyond giant deferred submission still uncertain;
SDK-native AutoKick returned Timeout without notified GPU fault previously.
Do NOT restore forced per-draw waits as permanent performance fix. No commits,
agents, unrelated edits; prior details below. User's intended goal remains
correct game rendering and usable performance with conformance support preserved.

## ASYNC LAUNCH TIMEOUT REPRODUCED — NEW BATCHING CANDIDATE UPLOADING

2026-10-20 user reports gpfifo entry append failure on alpha-repair build.
Retrieved build/package/switch/alpha-launch-failure-20261020 logs/env/crash.
Atmosphere 01792491558_03db12780bd84000.log MATCHES FF852425... build ID.
FirstPresent19.483, nativeappendtimeout19.492, 256 pending entries, command
0x50021C6A8 words1 flags80000200 threshold1792. Result0xD5C timeout; errorinfo
0x275C FileOperationFailed; no reported GPU fault, NOT proof none occurred.
Alpha repair not yet visually tested. Previous SYNC removal exposes startup
submission failure even with the known-good old shaders and unused UBOs skipped.

New candidate 97308ab4459d5fe413ba343a7082d9868d370361 builds/packaged.
Gpu::checkpoint submits through normal Gpu::flush(false) after16 completed
uploads/clears/copies/draws. Prevents accumulating startup commands untilfirst
Present; uses full SDK queue/ring flush rather than direct native AutoKick.
NO explicit per-operation CPU waits, no staging/command/deferred-free recycling
until existing frame fence. begin_list resets operation count, increments list_id
so next draw restores bindings. Readbacks/explicit SYNC still wait as before.
Removed ineffective wrapperthreshold1792; nativeAutoKick arguments now untouched,
error logging retained. Native limit override had NOT fixed timeout.
This is a reasoned CANDIDATE, real driver root/hardware success NOT established.
SDK source inspected: nativeAutoKick onlykicksoff, deko flush handles queue-ring
bookkeeping/fences. No claim GPU fault impossible or shader alpha repair proven.

Build973... and actual asyncbatch/list/flush CPUmodel testsPASS ASAN/UBSAN:
finite nativequeue, live staging/deferred state, restoreddescriptorbindings,
no explicit batch wait; readback wait preserved. Nativewrapper unchangedAutoKick/
atomic contract PASS ASAN/UBSAN. Alpha-compatibility test PASS. DiffcheckPASS.
Docs updated. Source Gpu checkpoint, Device sync hooks, nativewrapperthreshold
revert; newtests test_async_command_batches.py, adaptedtest_submission_batches.py.

Installer session75429 uploading NRO151099177B, SHA404b0a30c2ad84dc4422abb3a1961d17bea28e4b058e2e2d74578c0a770d0817.
Journal deployment-async-batches.json installedFALSE until upload+FULLreadbackSHA
verification and backup/rename. Tempcredentialscript /tmp/dirt2-install-async-batches.py.
Other shader/FFPfiles verified unchanged. Original pack preserved, finalenvonly
DIRT2_DEKO_LOG_PERF=1 preserved. No SYNC/selftest/resource flags. Symbols+zipready.
AfterinstalledTRUE askuserlaunch/checkmenusleavesFPS; reopenFTP iffails.
Delete tempcredential scripts /tmp/dirt2-launch-fetch.py andinstalleraftersuccess.
No agents/commits; no unrelated changes. Updateheaderwithcompleteddeployment.

## ALPHA REPAIR INSTALLED AND VERIFIED — AWAITING CONSOLE RUN

Game ff8524257bc59f569eb7b30672a9a62739aba62a successfully installed, full
NRO RETR SHA256 matches d9022dbf537406907b2285f5ed3c3301fcaf99427744ca48b8018fad3fe22c9e.
Journal build/package/switch/deployment-alpha-compatibility.json installedTRUE.
All shader/utility files verified against staged baseline before mutation; only
NRO and environment changed. Original known-good 897 pack remains installed.
NRO backup dirt2.nro.before-alpha-compat-20261020-120940. Symbols/package saved.
Final installed env is ONLY DIRT2_DEKO_LOG_PERF=1, lightweight 5s FPS reporting.
SYNC, SELFTEST, LOG_RESOURCES removed. Env upload/retrieval byte equality verified,
final contents/hash/backup in journal.performance_environment +
alpha-evidence-20261020/env-installed.txt. Package ZIP uses normal comment-only env,
intentionally no performance log flag. ZCull stays enabled. No active tools.

Asked user async to launch, check menu cropping + leaves, drive briefly, reopen
FTP; if launch crashes, retrieve logs/native GPU errors. Hardware visual repair,
normal async launch, and FPS improvement NOT yet verified. Source/local checks
are complete (details below). No changes to standalone Wine app; no agents/commits.
Deleted all credential-bearing temp FTP scripts for this turn.

## ALPHA/PERFORMANCE REPAIR BUILT — FTP VERIFICATION IN PROGRESS

2026-10-20 user supplied four more captures, reports alpha tiles in trees, menu
cropping, lower FPS. Retrieved ALL root images/raws/logs of four new dumps into
build/package/switch/alpha-evidence-20261020: 095613-f2501 Options,100015-f7007
Croatia flyover,100224-f7268 start,100512-f7654 race. ~92MiB. Geometry/texture
subdirectories not downloaded this turn. Log includes 393 alpha-enabled draws.
Installed full original pack hash cb34d9dbec8dbc98005496eb015d1e70572ffdc3e69c9f1b261abc7e3bfbe3aa.

Found compatibility mistake from diagnostic full-old-pack switch: renderer
unconditionally set native alpha compare Always because extended shaders do
alpha testing; original shaders have no epilogue. Thus BOTH tests disabled.
Source repair: PackMeta.has_draw_state distinguishes original bare v2 metadata
from extended flat-variant record (introduced together with draw-state wrappers).
All 897 old records have no extension; all 897 new records have extension.
Original pack now uses hardware alpha according to ALPHATESTENABLE/FUNC/REF,
except alpha-to-coverage; extended and FFP retain shader tests without duplicate
native testing. Skip unused fragment/vertex draw-state UBOs for original shaders.
Tree tiles explained by missing test; menu masking likely related but console
pixel validation still REQUIRED. Current tested old shader pack preserved.

Built ff8524257bc59f569eb7b30672a9a62739aba62a, packaged with old shader baseline.
Symbols saved symbols/<buildid>.elf+.symbols.txt. NRO SHA d9022dbf537406907b2285f5ed3c3301fcaf99427744ca48b8018fad3fe22c9e,
151099177B. Actual alpha-state emission across old/extended/FFP and ATOC/all8
comparisons/ref values PASS ASAN/UBSAN; raster/sample-mask regression PASS;
build and git diff --check PASS. New test test_alpha_pack_compatibility.py.
Added docs/D3D9_TESTS.md compatibility explanation. No unrelated edits/agents.

FPS confound confirmed: remote env STILL SYNC=1 SELFTEST=1 LOG_RESOURCES=1.
Planned install removes all3; adds ONLY LOG_PERF=1 (5s lightweight FPS stats),
no forced GPU stalls. ZCull already enabled (Gpu queue EnableZcull flag).
Ordinary async launch previously failed with NEW pack; old pack plus skipping
unused draw-state commands may help but is UNVERIFIED. Do not claim fixed FPS
or async launch before hardware run. Smaller native batch limit unchanged.

Current installer session75013: upload all payloads complete, RETR SHA verification
of 151MB NRO ongoing. Existing live files unchanged until BOTH payloads verified;
NRO final rename last. Journal deployment-alpha-compatibility.json installedFALSE
until completed. Temp /tmp/dirt2-install-alpha.py contains credentials; don'tprint.
After session completes, run /tmp/dirt2-install-fps.py (network escalated) to set
lightweight FPS-only env and record final contents/hash in journal + evidence.
Then ask user launch/inspect menu+trees+FPS/reopenFTP. Remove all temporary
credential scripts /tmp/dirt2-collect-alpha*.py, /tmp/dirt2-install-alpha.py,
/tmp/dirt2-install-fps.py after successful install. Collection sessions completed.
Update this header to installed when verification succeeds. No commits.

## HARDWARE CONFIRMATION: COMPLETE EARLIER SHADER PACK LOOKS CORRECT

2026-10-20 user confirms BOTH intro and Press Start look correct after full old
897-entry shader pack installation. Same current e6c0 NRO/backend and temporary
SYNC env. This strongly isolates observed visual regressions to shader-generation
changes, not missing earlier backend fixes. It does NOT prove normal asynchronous
launch is fixed, nor identify which translator/epilogue changes are wrong.
Keep this verified pack as game visual baseline; preserve standalone test app.
User asks about automated debugging / emulator. Existing dumps are NOT complete
API replay traces; propose deterministic capture replay + reference images and
real Switch batch runner, emulator supplemental because it reimplements GPU.

## TWO NEW CAPTURES ANALYZED — COMPLETE EARLIER SHADER PACK INSTALLED

2026-10-20 latest user: captured broken intro + Press Start luminosity; FTP ready.
Retrieved into build/package/switch/menu-ab-evidence/: current log/prev/env,
20261020-094334-f268-0 (intro) and 20261020-094413-f861-0 (Press Start).
Both dumps include all root images/raws, all draw descriptions/constants and
geometry/texture inventories. Also fetched selected tex000/001/007 inputs.
FTP installed hash confirmed prior THREE-shader A/B pack b2018f00... as expected.

Confirmed depth regression is gone with the three older native binaries:
522240 depth pixels before:367430 zeros; after:ZERO zeros, range .17982-.33203.
Detailed numeric report analysis.json in new evidence directory. Thus shader
path changes demonstrably regress depth, but exact epilogue/texture-code cause
is not yet isolated. Do not blame the compiler revision alone (old GLSL with
current UAM remained identical). Preserve conformance support in permanent fix.

Separate issue: scene itself is now very dim (RT03 RGB mean .024/.022/.017).
Game-supplied scene constants c55/c56 ~.1877/.1881 vs ~3.885/3.893 before,
a 20.7-fold difference. NOT fixed by final output brightness compensation.
RT05 luminance in ALPHA contains nonzero values even though RGB almost zero;
RT21/22 contain small nonzero red values (~5.6/65535). Avoid reporting luminance
chain fully zero. Exposure RT23 output [4,.047,1,1] matches low game scales.
Cause of adaptation/lighting change still UNRESOLVED, not necessarily another
independent compiler bug. Captured intro's copy source tex000 fmt35 is already
all black, before its copy shader; static legal UI is partially visible.

Installed FULL OLD game shader pack build/shaders/dx9/dirt2.dkshpack (1801260B),
all 897 bytecode identities match current pack. Verified upload via RETR exact
byte equality. This is controlled diagnostic comparison with current e6c0 NRO,
not a permanent conformance feature rollback. Manifest with SHA/backup name:
build/package/switch/menu-ab-evidence/full-shader-comparison/manifest.json.
Both preceding shader packs remain backed up on console. Standalone conformance
app unchanged. Local standard game package still contains newer shaders; do NOT
accidentally overwrite this A/B pack before user tests. No NRO or env mutations.
Asked user async to check intro/Press Start brightness and capture/reopen FTP.
Temporary SYNC/SELFTEST/LOG_RESOURCES still active; must restore normal env after
launch root repair, not forgotten. Game rendering and async launch unresolved.
No active tool sessions, no agents/commits. Temporary credential FTP scripts deleted.

## CURRENT MENU REGRESSION — THREE-SHADER HARDWARE COMPARISON INSTALLED

2026-10-20. User reached menu with temporary SYNC/selftest env, but reported
severe renewed graphical corruption and supplied a Press Start hotkey dump.
Evidence: build/package/switch/menu-regression-20261020-094658/.
Remote dump gpu-dumps/20261020-074540-f1583-0. Downloaded all 25 render-target
BMP/raw images, screen, capture.txt, 616 draw descriptions and 616 constants.
Geometry vertex/index data and sampled texture blobs are NOT downloaded yet;
local inventory files list them. No crash/root synchronization repair yet.

Scene RT03 is mostly clean. R32F depth RT02 contains extensive zero/block
patterns; earlier clean captures manual-evidence-3/20261019-173105-f14447-0
have no zero depth pixels. Final shader draw599 PS c3f383cae33a uses this depth.
An analytical CPU evaluation of its current GLSL with captured constants/RTs
reproduces the block noise (CPU/GPU means .388681/.388555 red, .314343/.314295
green, .238824/.238754 blue; mean absolute difference .01284 including UI).
Reference script /tmp/dirt2-composite-reference.py; PNG in dump/. This supports
bad depth input rather than the final composite creating the noise.

Three MRT pixel shaders account for the 208 depth draws: e6b631a7163b,
e80dc3fc05c7, 0dd3a9fe6a08. Current sources retain the old replicated-depth
workaround. New alpha/sRGB/shadow wrappers substantially change native code.
Recompiling OLD GLSL with CURRENT UAM yields byte-identical OLD DKSH for all
three, so compiler-version change alone is not established as the cause.
The newly added runtime epilogue/texture paths are SUSPECTED, NOT PROVEN.

Installed a diagnostic pack replacing ONLY these 3 native binaries with the
old known-working binaries; all other 894 entries and current metadata intact.
No NRO or environment changes this turn. FTP upload then RETR byte equality
verified. Manifest (installed true, backup filename, SHA) in evidence/mrt-ab/.
This is a controlled A/B, NOT a permanent feature rollback or a proven fix.
Asked user async to launch, check Press Start, dump again and reopen FTP.
Await hardware feedback before choosing a permanent repair. If it resolves the
noise, isolate alpha epilogue vs shadow sampler paths / native MRT exports;
preserve required D3D9 state behavior rather than removing conformance support.

Current NRO still e6c0c7057ebb1432564d6e1c9f00adb4fa4aacc6. Temporary env
DIRT2_DEKO_SYNC=1, SELFTEST=1, LOG_RESOURCES=1 still active intentionally for
launch diagnosis; MUST restore normal env after root repair. A/B pack differs
from the standard staged package: do not accidentally overwrite it while user
tests. Original pack retained remotely using manifest backup name. Clean game
rendering and normal asynchronous launch both remain unverified/unfixed.
No agents, commits, or active tools. Removed credential-bearing temp FTP scripts.

## GPU SUBMISSION TIMEOUT IDENTIFIED —TEMPORARY SYNC/SELFTEST RUN

Userreported samegpFifoappendmessage ongamee6c0... . Retrievedevidence:
build/package/switch/submission-failure-20261020-093254 (dirt2.log,prev,env).
AtfirstPresent19.650s nativeappendfailed19.658 at256entryAutoKick:
commandIOVA0x50021C6A8 words1 flags80000200 threshold1792,
Result0xD5C=Module_LibnxNvidia348/description6=LibnxNvidiaError_Timeout.
NvErrorqueryResult0x275C (FileOperationFailed), so NOdriverfaultdetailavailable;
notproofthereisnoGPUfault. Nativefd25755652/fence16365127+0 notassumedcorrupt:
NvFDcanbeencodedandfencecountersglobal. SmallerbatchdidNOTfixcause. Revert
ineffectivebatchlimitonceactualrootfixready; keepnativeerrorlogging useful.

No newNROthisturn. Installed temporaryenv viaFTP192.168.1.148:5000, bounden0:
DIRT2_DEKO_SYNC=1, DIRT2_DEKO_SELFTEST=1, DIRT2_DEKO_LOG_RESOURCES=1.
SYNC waitsaftereveryupload/clear/blit/draw; firstwaitisdummy1x1uploadininit,
then180clearedswapimageframes beforegame. Usermayseebriefcolor test; slowrun.
Goalidentifyfirstblockingoperation/basicGPUvsactualgamework; diagnosticsnotfix.
Need userlaunchuntilmenuorerror, thenFTP. RESTORENORMALENV afterdiagnostic!
Backupname/installedverification in evidence/trace-deployment.json, priornormal
contents env-before-trace.txt. InstallationCOMPLETE/retrievedbytesverified;
backupenv.txt.before-submission-trace-20261020-093723. Noactivetools.
GameNROe6/shaders unchanged.
Device::sync doesn't logbeforeflush; ifdriverabortinsideflush, use lastresource
log andfirstknowncall. wait_fence reportsoperationifGPUwaitlastsover2s. Avoid
claiming allremainingtests/orgamefixed. No agents/commits.

## LAUNCH REPAIR INSTALLED AND SHA256 VERIFIED

Gamebuilde6c0c7057ebb1432564d6e1c9f00adb4fa4aacc6 successfullyinstalled after
userWiFichange. NROfullupload+retrieveSHA256verified:
d8f430c9863da66f1a1109768098bc1b276a68f83181b395e8602bff76ba8eab.
Backup:dirt2.nro.before-launch-submission-20261020-092450.
Journal build/package/switch/deployment-launch-submission.json installedTRUE;
atomicbackup/rename andfinalsizeverified. Other12filesunchanged, matching897
shaderpack/FFP. Newpackage+currentmanifest+ELF/symbolssavedlocally.
Newcandidate bounds AutoKickstartupbatches to256, linksvalidateddeko3dd and
logsunderlyingnativeGPUsubmissionerrors. ActualconsolelaunchstillUNVERIFIED.
User should launchDiRT2via titletakeover; reopenFTPifcrashes. DoNOTclaimfixed
untilhardwareconfirms. Originalcrash651b...firstGPUframe preservedinpriornotes.
Standalone33f255...suite stillawaitsfullrunresults; notupdatedthisturn.
Noactive tools/agents/commits. Deletedtemporarycredential-bearinginstaller.

## LAUNCH REPAIR UPLOADING AFTER WIFI CHANGE

Userconfirmed192.168.1.148:5000. InitialdefaultVPNtimeouts/per-socketWiFirefused.
UserchangedWiFi; Macnow192.168.1.94/en0, Switchhostrouteen0; BOTHdefaultand
bound-WiFisocketsget220FTPbanner. Installer /tmp/dirt2-install-launch-repair.py
running session54777 withper-socketen0 (controlandpassive), NROupload+retrieve
SHA256verification, backup+rename+SIZEcheck. Targetbuilde6c0c7057ebb1432564d6e1c9f00adb4fa4aacc6.
Checksessionanddeployment-launch-submission.json installedtruebeforeclaiming
complete. Nochanges/commits/agents. Latestusersteering: switchedWiFi, try now.

## GAME LAUNCH REPAIR READY —NEED CURRENT FTP ENDPOINT

Retrieved build/package/switch/launch-crash-20261020-011657, latestAtmosphere
01792452023_03db12780bd84000.log. Matches game651b4560... buildID. Log ends first
Present19.709s. CrashUserBreak in deko Queue::appendGpfifoEntries, notgameguest
CPU/shaderload. nvGpuChannelAppendEntry failed; checkError false; release
RaiseError(1) abort loses actualResult. No provenGPUmethodfault/rootcauseyet.
Report+ELFsymbolization maps append at0xda19c, SubmitCommands0xdaeb8,Present.

Newcandidatee6c0c7057ebb1432564d6e1c9f00adb4fa4aacc6 built andpackaged. Adds
shims/d3d9/deko/submission.cpp linkerwrappers fornvGpuChannelAppendEntry/Kickoff:
nonzero AutoKickthreshold increased to1792, bounding permitted nativebatches to
256 ratherthan2040; threshold0 atomicboundaries andpayload preserved. Existing
SDK's own AutoKick boundary only; nativefenceaccounting unchanged. Exacterrors,
commandargs, pendingentries, fence, nativeNvErrorwords logged beforeSDKabort.
HardwarelaunchFIXUNVERIFIED; limitedbatch iscandidate notprovenrootcause.
GameDIRT2_DEKO_VALIDATION defaultONlinksdeko3dd, matchesdiagnosticsuite;
previousreleaselibrary hidinvalidAPIargument andchannel details. OptionOFF
retainsrelease library whenappropriate. Testtarget alsolinkssubmission wrappers
onnextbuild (notrebuilt/deployedthisturn; installedsuite33f255stillpending).
Newtest_submission_batches.pyactualwrappers passed ASAN/UBSAN: 10kcommands,
256entryAutoKick limit, noatomicboundarysplit, payload/strongerreserve/error
propagationpreserved. Existing21CPUtestsunchangedsincepreviouspasses.

NewgameonlyNROchanged: assertsother12stagedfilesidenticalSHA toinstalledmanifest;
current897shaderpack/FFP unchanged. symbols/<newbuildid>.elf+.symbols.txtsaved.
Package build/package/switch/DiRT2-switch.zip regenerated. FTPconnecttimedout
beforeanyupload/session39878finished. Journaldeployment-launch-submission.json
installedFALSE. Askeduserasync toreopenFTP192.168.1.148andkeepawake. Retry
python3 /tmp/dirt2-install-launch-repair.py afterFTPready (containscredentials,
doNOTprintscript orcopytorepo;deleteafterdeployment). Itchecksother12files
unchanged, stagesNROwithprogress, retrieveSHA256verifies, backup+rename+SIZEcheck.
UserrepliedFTPready, butinstaller73273 timedoutbeforeupload. FoundMacLAN
192.168.1.169/en0, while192.168.1.0/24routedviaVPNutun6/10.255.0.189. Per-socket
IP_BOUND_IF=en0 avoidsVPNwithoutchangingglobalroutes. DirectWi-FiTCPtoSwitch
192.168.1.148port5000REFUSED (also21/2121refused). AskeduserasyncforIP/port
currentlyshownonSwitch. Installertemp now scopesbothFTPcontrol/passivedata
sockets toen0. Updatehost/portifuserreportschange, retryinstallerafterready.
PackageNROhashandmatchingELF/symbolsverifiedlocally. JournalinstalledFALSE;
NOnewupload, previousgame651b...stillinstalled. Noactivetools/agents/commits.

## CURRENT D3D9 GAME INSTALLED AND VERIFIED

Game build651b4560f4a4427201727939d124a89487dbe295 from currentbackend, including
combinedrenderhazardcandidate33f255...suitecode. Switchgamebuildpassed.
Fresh game shaderpack build/shaders/game-render-hazards/dirt2.dkshpack:897/897
compile andSPIRVvalidatePASS. CurrentFFP/utilities rebuilt. Package normalenv
(no diagnosticflags); originalremoteenvwasalso normal. symbols/<buildid>.elf
and.symbols.txtsaved underbuild/package/switch. Manifestcurrent-d3d9-manifest.json
verifiesZIPagainststage. tools/scripts/package_switch.sh now accepts optional
thirdargument shaderpackpath to avoid packagingstale defaultdx9pack.
Gamepackage build/package/switch/DiRT2-switch.zip. FTPinstallationCOMPLETE:
all13filesstaged/retrievedSHA256verified, oldfilesbackedup+renamed,NROlast,
finalsizeschecked. Journal build/package/switch/deployment-current-d3d9.json
installed=true. Remotebackup suffix before-current-d3d9-20261020-011144.
Remotepriorenv/logpreservedlocallygame-render-hazards-before.
Gameassets/savefolders untouched. CurrentgamevisualcorrectnesshardwareUNTESTED.
No active tools/agents/commits. User can launchgame via title takeover.
Standalonefull144 ordinarycandidate33f255...stillawaitsrunresults; don'tclaim
141 diagnosticpasses establishordinaryRESZfix.21localchecksalreadyPASS.

## PIXEL EVIDENCE RETRIEVED; COMBINED HAZARD REPAIR INSTALLED

Evidence build/wine-d3d9-results/switch-pixel-evidence-20261020-010504.
Diagnostic buildfc639f... completed269:141pass/2fail/1partial/125notselected.
RESZ82 passed ONLY WITH extra DMA snapshots; not an established ordinary-build
pass.65 and71 stillfail. No crash/interruptedcase.197YUVtwo skips remain.

Numerical findings:
-65 DS2partialclears correct:0/.5/1.0 packeddepth; everyDS2→DS1DMAcopy retains
all sampledvalues, including.5 at(400,60),0 at(560,450). Subsequentgreenandblue
bothdraw everywhere evenwhereDS1depth0; storeddepthunchanged. Suspect native
cullsummary afterDMA; plain DkInvalidateFlags_Zcull hadn't fixed it.
-71 RGBA32F draw hasRGB.73664695 at(7,7), correctmanualsRGB; readbackdraw gives
black upperprobes andcorrectBCBCBC bottom-right. Existingprobe clamped all8
small-image points to7,7, so cannot claim all source pixels known. Need newgrid
ifnotfixed. Render-to-sampling publication candidate, not provenrootcause.
-82 nonMSAADSgradient correct(.125,.625,.875), RESZdestinationmatches, final
PSreadbackcorrectwithsnapshots. DMAflush/order changes likelyexposemissing
publication ratherthanshaderarithmetic.

New candidatebuild33f2558d3ddb6a79da4c1e2c37e1b4982076da18:
blit depthDMA rebindssource then destination, invoking SDK's full conditional
Zcull target-change reset(0x19), preserving stencil copiedbyDMA. Zcull staysON.
flush_targets completesFragments+Image|L2Cache; resolve_depth publishesprior
fragmentwriteswithL2CachebeforeFull/Image sampling barrier. HardwareUNVERIFIED.
Pixelprobes now require rootpixel-probe.txt trigger (notpresent); ordinaryfull144
run mustpass withoutdiagnosticDMA. Small-imageprobegrid improved forfuture.
21CPUregressionsPASS; newtest_render_hazards actualmethods withnativecache
models, notGPUproof. Existingclear-regionsmock supportsoptionalprobe no-op;
alloriginalWineexpectedbodies unchanged. Switchbuild/package passed.
Validation build/wine-d3d9-results/pixel-evidence-repair-local-validation.json.
Installed/retrievedSHA256verified18files,NROlast,finalsizesandnext0verified.
Journal build/package/switch-wine/deployment-render-hazards.json. Priorresults
archived results.before-render-hazards-20261020-010952. Full144selection retained,
no activepixel-probetrigger, gameunchanged. User should runfullsuiteonce thenFTP.
No active tools/agents/commits.

## PIXEL-EVIDENCE BUILD INSTALLED —WAITING FOR FULL144 RUN

Buildfc639f4ee7260d4e3cc9290426e9dd53cbd3f39c installed/retrievedSHA256
verified18files,NROlast,sizesandnext0verified. Journal:
build/package/switch-wine/deployment-pixel-evidence.json. Remote priorresults
preserved as results.before-pixel-evidence-20261020-010032. Original144selection
unchanged. This is a DIAGNOSTIC build, no new speculative rendering repair.
main.cpp enables DIRT2_DEKO_PIXEL_PROBE only for65,71,82. device.cpp queues
8 one-pixel DMA snapshots at Clear/draw/blit/resolve boundaries, inspects only
after frame completion via deferred callbacks; no CPUwait/listreset. Logs raw
words/floats sample0..3=(80/240/400/560,60),4..7=sameX,y450; clampedtoextent
for8x8targets. SkipsMSAA and other blocksize/bytes. Maximum96snapshots/case.
Extra DMA/barriers may affect synchronization: passing with probe alone does
NOT prove ordinary build fixed. Collect suite.log pixel-probe entries and
compare depthcopysource/destination, RGBA32clear/draw/sample. Allfailureexpected
bodies unchanged. Latestthree relevantCPUchecks gpu_shutdown,texture_binding,
gpu_state_paths passed ASAN/UBSAN;Switchbuild+packagepassed. Game unchanged.
No running tools/agents/commits. Ask user fullrun once then reopenFTP.

## FULL144 RETEST COMPLETE —140PASS/3FAIL/1PARTIAL

Evidence: build/wine-d3d9-results/switch-parity-repair-20261019-225628.
Build45dfa0aa4182dfaac86cd9c416f0fe7ef383bd76: complete269, no pending,
140pass/3fail/1partial/125notselected. Seven additional passes, no regressions,
crashes, missing shaders or GPU faults. Remaining65 depth_blit_test (400,60
blue instead of green),71 srgbwrite_format_test RGBA32F black,82 resz_test
later nonMSAA-source check (560,450 red.5 instead of.875).197 packedYUV2skips.
Continue investigating all remaining failures; original144selection retained.
Game unchanged. Do not claim parity achieved. Next diagnostic needs raw depth
and floating-point pixel snapshots at command boundaries without CPU waits.

## NEXT COMBINED REPAIR INSTALLED — WAITING FOR FULL144 HARDWARE RETEST

Build45dfa0aa4182dfaac86cd9c416f0fe7ef383bd76 installed+verified18files
(stagingretrieveSHA256, renameNROlast, finalsizesandcursorchecked).
Journal: build/package/switch-wine/deployment-final-parity-repairs.json.
Validation: build/package/switch-wine/final-parity-validation.json.
Remotepriorresultsbackup: results.before-parity-repair-20261019-225301.
Originalreference-pass144selection/checksum retained; newresults/next=0.
No pending/probe. User should launchD3D9WineTests via titletakeover, letfullrun
finishthenFTP (orfirstunexpectederror). Previoushardware133PASS/10FAIL/1partial
with14newpassesandno regressions/crashes/missingshaders/GPUfaults preserved.
Evidence build/wine-d3d9-results/switch-combined-20261019-224036.

All20localregressionsPASS, all897gameshaders compile+SPIRVvalidate; testpack154
compiledfrom159,5sameinvalid/unsupportedfixtures. All12Mojo patchesroundtrip
verifiedagainstgeneratedsource. LastSetDepthStencilSurfacehazardchangebuilt
andstateblock+texturebindingchecksrepassed. No furtheredits/builds needed.
Newcandidatehardwareunknown; doNOTclaimalltestsfixed. PackedYUVpartialskip
stillunsupported. GameNRO/pack unchanged. Noactive tools/agents/commits.

## NEXT COMBINED REPAIR LOCALLY VALIDATED — BUILD/DEPLOY PENDING

New testpack build/shaders/wine-d3d9-final-parity:154compiled/159candidates,
5sameinvalid/unsupportedfixtures. Full897game shadercompile+SPIRVvalidatePASS.
All20regression scripts PASS; expanded realshaderpipelinefixture now10cases:
PS1depth+color metadata,PS1.4t5 input,legacy3D/cubesampling,mixeddirectC7/address,
coissuedCND,SM2TEXKILL4channels, oldoutputmask/flat/DSY/projectionregressions.
AllMojo patchesroundtrip reverse/apply checkrunning session92139 (inspectfinal).
Compilerentrypointimplicit-inputcount widenedthroughPS1.4andarray8(patch0011).
Pre1.4coissuedRGB CNDselectssecondsource;ordinaryalphaandPS1.4normal(patch0012).

Latestbackendchange afterall20checks: SetDepthStencilSurface also sets
copy_barrier_ondetach/reattach toensurerender→samplingvisibility, analogousRT.
Rerunrelevantstateblocks/texturebinding, finalSwitchbuildthenpackage+upload.
Nohardwarevalidationofthisnewcandidateyet. Installedstill014930...previousrun.
GameLUTnewsourceguard847a65...acceptedafterprovingonlycommentchangefromold
b0d857...source. Gamepacklocalonly; keepgameunchangeduntiltestparity.
All10remainingfailuresnowhaveconcretecandidatefixes; packedYUV197still2skips.
DoNOTclaimallpasswithoutfull144hardware rerun. Keepselection/checksumunchanged.

## SECOND COMBINED RUN COMPLETE —133PASS/10FAIL/1PARTIAL; NEXT REPAIR IN PROGRESS

Evidence: build/wine-d3d9-results/switch-combined-20261019-224036.
Complete269/no pending;133PASS/10FAIL/1partialskip/125notselected.14additional
casespassed, no regressions/crashes. UserauthorizescontinuedallMacparityfixes.

Remainingcases37MOVaddress,47TEXDEPTH,48TEXKILL_SM2,49volumeV16U16,51CND,
65depthblit,68shadow,71srgbwriteRGBA32F,82RESZ,101colorclamp;197YUVpartial.
New root fixes inprogress:
- patch0010: directC7 within implicit bank was lowered asC0; corrected uniform
  registeroffset +CTABrelativebase handling. FixPS1.4 Textureglobalinit (was
  uninitializedprivatet0). TEXKILL SM2tests4channelsvs3onSM1. ClampPS1/2color
  inputsbeforearithmetic; PS3unclamped. PersistentMojo patch0010.
- SM1.1implicit sampler is nowruntime-dimension generic: JSONtypelegacy=3;
  bind2Dslots0..5,volume8..13,cube16..21,shadow24..29. Shader branchesvia
  floatBitsToUint(FragmentFogParams.w) packed2bitdimensions. NoUBOsizechange.
  Otherprogrammableshadowuses16..31 unchanged. Pendingcompiler/cpuvalidation.
- RESZ actualATI magic0x7fa05000 acceptedandretainedforGetRenderState, aswell
  asfourcc. Oldtriggeronlyfourcc, so testneverresolved.
- RTdetach nowsets copy_barrier_ forsampleafterrender, andcopybarrier invalidates
  ZCull aswellasImage. Neededdepthcopyvisibility, floatingRTtexturevisibility.
- TEXDEPTH stillmaskedcoloroff: previousSM1 implicitCOLOR metadata addedonly
  ifoutputcount0. TEXDEPTHpd.outputscontainsDEPTH, so mask0. Nowalwaysadd
  implicitCOLOR0unlessalreadyexplicit, preserveDEPTHoutput too. Newtoolbuild
  neededafterthislatestchange; currentlyoldpackbuildsession43642running.

Currentnewpack output build/shaders/wine-d3d9-final-parity. Mustrebuildwithlatest
implicitcolor+depthfix. No deploymentthisturnyet. Installedprevious014930...;
userFTPavailable. Keepfull144selection, allprior evidence. UpdateMojo.patched
SHA1 after0010 beforebuild_shader_tools toavoidunnecessarydownload, orrebuild
pinnedtoolscriptnormally. GameLUTsourcehashguard willneednewacceptedhash
becauseFogParamscommentchanged; inspectbeforeaccepting. Noagents/commits.

## COMBINED REPAIR BUILD INSTALLED — WAITING FOR FULL144 HARDWARE RUN

Installed + retrieved SHA256 verified all18 files, NROlast, final sizes/cursor
verified. Build0149300a50aaa9932328520d2669f88d353aff54.
Journal: build/package/switch-wine/deployment-combined-repairs.json.
Validation: build/package/switch-wine/combined-validation.json.
Previousresults archived remotely as results.before-combined-20261019-223705.
New results/next.txt=0, original144 reference selection retained. No pendingcase
or probe trigger. User should launch D3D9 Wine Tests via title takeover once,
let fullrunfinish, reopenFTP after returninghomebrew (orfirstunexpectederror).
Priorcompletedrun119PASS/24FAIL/1partialskip preserved. Combinedcandidate
hardware results unknown; neverclaimalltestsfixedbeforecollectingrerun.
All20localregressionchecks pass;897game shaders compiled+SPIRVvalidated,
154test shaderscompiledincludingallNRMbodies;5rejectedfixturesremain.
No active tools, no agents, no commits, installedgame unchanged.

## COMBINED CANDIDATE VALIDATED LOCALLY — PACKAGING / DEPLOY NEXT

All24 assertion failures have corresponding repairs in this candidate; not yet
hardwareverified. Remaining partial-skip197 is real missing YUY2/UYVY GPU support
(two skips), not silently changed into a pass. No Wine expected values changed.
New full shader pipeline regression passes. All19 previous/extended CPU regression
scripts +new back-buffer test PASS (20total). Actual GPU descriptor regression
checks resident LOD/sRGB caches, ID lifetime and depth TIC swizzles. Full897game
shaders compile; all897 SPIRV validate. Testpack154/159 candidates compiled;5
rejected fixture shaders preserved. NRM now collects all10 runtime bodies.

New UAM root: DSY emitted MUL input by unbound CONST[0][0].x, which made DSY
zero. Patched pinned UAM5a5afc2bae8b55409ab36ba45be63fcb73f68993 avoids GL
Y-flip state; TGSI verifies DDY without CONST0. Persistent patches and build
script tools/scripts/build_uam_tools.sh. Mako1.3.10 installed onlyprojectvenv.
build_shader_tools.sh now builds both host tools. New census requires patched
UAM build/tools/uam/uam, never silently uses old SDKcompiler.

Programmable shadow mode now preserves full z/q sample coordinates via
D3D9CompilerGLSL override; declares shadow sampler slots16..31 and branches on
per-texture bits16..31 in fragment Control.w. Ordinary INTZ/DF formats bypass
comparison. Backend binds comparison samplers and mode bits. Projected samples
use fullvec4 explicitdivision (persistentMojo patch0009) so znotdiscarded.
Offline flat variants append length+DKSH to version2 metadata; oldpacksreadable.
Device chooses flat variant onlywhen D3DRS_SHADEMODE=FLAT. Constant/smooth
texturecoordinates remain unchanged. Additional swapchains own privatebackbuffer.
Flip keeps Surface COMidentity while rotating GpuImage storage; supports3buffers.

New game shader pack is ONLY local validation; do not deploygamepack to oldgame
NRO because newshaderinterfaces require matchingbackend. Currentgame untouched.
Use build/shaders/wine-d3d9-combined/dirt2.dkshpack for standalone nextpackage.
Reference-pass.txt stilloriginal144 entries/checksum. Package then atomicFTP
upload+retrievehash+rename installNROlast, archiveoldresults, freshnext0.
No render probe trigger necessary. No commits or agents.

## FULL MAC-PARITY RUN COMPLETE — COMBINED REPAIR IN PROGRESS

Evidence: build/wine-d3d9-results/switch-mac-comprehensive-complete-20261019-221959.
Cursor/complete269; selected144:119PASS,24FAIL,1partial-skip. No pending case.
Four rendering selfchecks PASS; scene154 and MSAA case3 PASS on hardware.
Installed af4e1e784853305b74a758365e389fbb0fbf02d1. Game remains unchanged.

New shared root found: Mojo SM1 implicit r0 output had no Location decoration;
metadata outputs[] was empty, device color_outputs mask0 suppressed writes,
and pixel_epilogue skipped alpha/fog/clamp/sRGB. New persistent patch0008
adds Location0; translator metadata reports implicitCOLOR0. Rebuilt hosttool.
Combined repair now also includes viewport epsilon, streamfreq validation,
whole-surface-only depth StretchRect, pinned buffer lock hints, NOOVERWRITE
precedence, managed texture LOD views, missing VS diffuse white, additional
swapchain, offline programmable flat interpolation variants, depth TIC swizzles.
These changes are NOT yet installed/hardwareverified. Compiler/regression
verification ongoing; all five deliberate/unsupported shader fixtures retained.
New runtime NRM shader downloaded under evidence/shaders, merged into
build/shaders/wine-d3d9-runtime-combined. Testpack output wine-d3d9-combined.
Do not use old staged pack! Copy newly built combined pack before packaging.
No agents, no commits. User in homebrew screen, FTP accessible.

## SURFACE INIT FIX INSTALLED — WAITING FOR RESUME

Journal confirms af4e1e784853305b74a758365e389fbb0fbf02d1 installed/verified.
All completed results preserved (73passes,22fails), pending/cursor backed up,
next154 queues interrupted scene test for retry then remainingfullsuite.
No tools running. User should launch once and FTP at completion/firsterror.
Exact errors normally available in suite.log; typing not required with FTPready.
No commits/game changes. Hardwarevalidation of latest fix pending.

## OFFSCREEN SURFACE INIT FIX — UPLOAD IN PROGRESS

New user error: image used as render target must have UsageRender set.
Evidence: build/wine-d3d9-results/switch-render-usage-error-20261019-221511.
Full run now reached inventory154 with 73PASS /22FAIL /59not-selected, no further
hard stop since case3 MSAA repair. So physical-MSAA repair hardwarevalidated;
case3 PASS. Four render selfchecks passed on prior build. Do not lose this data.
Fatal at device.test_scene creating DEFAULT offscreen plain surfaces (Usage0).
Surface constructor unconditionally clears them via render target binding, but
image_flags(Usage0) omitted UsageRender. Fixed standalone GPU surface allocation
to add internal render capability for block1 formats, keep API Usage0 untouched.
Unrenderable compressed surfaces skip render initialization clear. CPU-only path
unchanged. Dummy2D also render-capable because optional self-test renders into it.
No assertions/expected results modified.

New actual Surface-constructor regression test verifies Usage0, physical flags,
RT/depth clears, compressed no-render-clear, CPU-only allocation; ASAN/UBSAN pass.
Image layout/MSAA and Clear-regions tests pass; Switch build and diff-check pass.
Buildaf4e1e784853305b74a758365e389fbb0fbf02d1 packaged + matching ELF/nm.
Uploading session42100; journal deployment-surface-initialization-fix.json.
Verify final installed=true before telling user installed. All results retained;
oldcursor155/pending154 backed up, retry154, then continue full144selection.
No fresh probe needed. No game NRO/assets changed, no commits.

User also asked whether to type errors. Answered: exact deko messages are in
suite.log after FTP opens, so “error, FTP ready” normally sufficient. Typing
helps when startup precedes logging or on-screen text differs. Continue task.

Remaining22failed names/messages are in downloaded results. After remaining49
selected cases finish, fix all remaining failures as one combined repair instead
of asking user small batches. Current failures include maxmip LOD, flat shading,
alpha compare, inverted viewport, legacy MOV/TEXDEPTH/TEXKILL/CND/clamp/NRM,
V16U16, stream validation, depthblit validation, INTZ/shadow/SRGB/RESZ, flip,
float color clamp, DSY sign, map flags, default missing diffuse. Need full results
before final combined fix. No shader GPU fault issue now; all are assertions.

## MSAA PHYSICAL LAYOUT FIX INSTALLED — FULL SUITE RESUMES CASE3

User reported deko resolve error: must specify multisampled source and
non-multisampled destination. FTP was accessible; evidence:
build/wine-d3d9-results/switch-msaa-error-20261019-220854.
CRITICAL POSITIVE RESULT: render-probe-complete.txt confirms ALL FOUR render
checks passed after sample-mask fix. Shared GPU fault is hardware-verified fixed.
Full suite started automatically: case0 sanity PASS, case3 MSAA stretch interrupted
in dkCmdBufResolveImage validation. Cursor4/pending3 before repair.

Root cause: GpuImage::create retained requested ms_mode but passed ordinary
DkImageType_2D to layout maker. deko only honors msMode for type2DMS/2DMSArray.
Fixed physical lm.type selection while preserving logical resource dimension.
Rejects multisample3D/cube requests. Regular1x paths unchanged. Extended actual
image-allocation test covers genuine physical2DMS/2DMSArray for2x/4x/8x plus invalid
dimensions. Image-creation, GPU-state-path, raster-mask ASAN/UBSAN checks pass;
Switch build/diff-check pass. No other changes.

Installed/verifed new test NRO + matching source:
25234a540e11a97f05a92e9754d55f9c39571853.
Journal deployment-msaa-layout-fix.json, matching ELF/nm/manifest/package saved.
Uploaded temp files retrieved/SHA256 checked, renamed and sizes checked. All
completed results retained. Old cursor/pending backed up remotely, next3 and
no pending marker queue interrupted case3 for retry. No rendering-probe trigger:
already passed; next launch directly retries MSAA test then continuesall144.
Ask launch once and FTP at first error/completion. MSAA hardwarevalidation
pending. No game NRO/assets changed, no commits. No tools running.

## SAMPLE MASK FIX INSTALLED — WAITING FOR FRESH FULL RUN

Final installed/retrieved-SHA256-verified build:
9a4bf4ad5000317477d2e2873415d76822801abf.
Journal deployment-sample-mask-fix.json confirms installation and archive:
/sdmc:/switch/d3d9-wine/results.before-sample-mask-fix-20261019-220552.
All old results retained. Fresh cursor0 + render-probe trigger1 verified.
Original144selection hash unchanged. User should launch ONE time; four rendering
checks run first and automatically launch full Wine selection if all pass.
If error or return to HomebrewMenu, reopenFTP before relaunch. Retrieve probe log
and suite results. Expected root GPUfault fix: samplemaskmethod3ef now0xffff max.
Hardware validation still pending. No running tools, no commits, game unchanged.

## EXACT SHARED GPU FAULT FOUND — SAMPLE MASK FIX UPLOADING

Latest probe downloaded to build/wine-d3d9-results/switch-render-probe-latest.
All four shader pairs, including simple/simple, faulted identically:
GPU method error irq0x00100000, [b197:3ef]=0xffffffff.
Decoded against installed deko SDK source maxwell/engine_3d.def line267:
0x3EF MultisampleSampleMask array[4]. GPU driver wrapper gpu_3d_ms.cpp forwards
mask unchanged to all four registers; SDK defaults them to0xffff (gpu_3d_base.cpp).
Our comprehensive build newly called dkCmdBufSetSampleMask with D3D9 default
0xffffffff on every draw; reserved upper16 bits are rejected by real hardware.
This explains shared queue faults and invalid black/stale pixel results; NOT a
specific FFP shader-combination failure. Volume-flag validation fix remains valid.

Fixed Device::flush_raster_state to emit rs_[MULTISAMPLEMASK]&0xffff without
changing stored D3D9 DWORD state. Also fixed depth-copy helper ~0u->0xffff.
New actual raster-emission regression tests DWORD masks across1x/2x/4x/8x,
rejects any reserved bits at SDK boundary, confirms original API state unchanged.
ASAN/UBSAN passes. GPU-state paths and shader lifetime tests pass. Switch build
and diff-check pass. No remaining unmasked SetSampleMask calls in backend.

New build9a4bf4ad5000317477d2e2873415d76822801abf.
render_probe now returns bool success (colors exactly green/simple PS or white/
FFP PS, no GPU errors). After all4 succeed, runner arms automatic next-load to
resume original Wine suite. On failure returnsHomebrewMenu/error with logs.
90-second watchdog still enabled. Diagnostic results are NOT Wine passes.

FTP upload in progress session38487. Journal deployment-sample-mask-fix.json
tracks final state. After NRO/README/source temp retrieval SHA256 verification,
rename/install-size check, entire prior results directory will be archived with
results.before-sample-mask-fix timestamp. Fresh results cursor0 and probe trigger1;
all144 ORIGINAL Mac-passing selection verified SHA256 unchanged. This full rerun
is necessary because prior draw results came from faulted GPU code. All old
evidence retained remotely and key files locally. Game NRO/pack unchanged.
When installed verified, ask launch ONCE; rendering checks should automatically
continue all144 if they pass; at first error user should stop/reopenFTP. No claim
hardware success until new results. Matching ELF/nm/package saved. No commits.

## FINAL RENDER PROBE INSTALLED — WAITING FOR USER RUN

Verified deployment journal confirms final build
5a9db37ec9d603f982fbcd51d1d17f5f308ae89f installed. Uploaded NRO and matching
source ZIP were retrieved and SHA256 verified as temporary files, renamed into
place, and installed sizes checked. Both diagnostic shader assets previously
retrieved/hash-verified. Probe trigger1 verified. Wine cursor21/results unchanged.
No running tools. Ask ONE launch, then FTP on completion or first error without
relaunch. Need render-probe.log and render-probe-complete.txt/raw fault0272.
Corrected full FFP matrix UBO range + shader cache GPU-generation lifetime;
hardware effects NOT verified. Final render probe includes 90-second watchdog
and same depth setup as failing case20. No game files changed, no commits.

## FINAL RENDER-PROBE CANDIDATE — UPLOAD IN PROGRESS

Final diagnostic build is 5a9db37ec9d603f982fbcd51d1d17f5f308ae89f.
Earlier intermediate 40f62506... was installed but NOT requested to run. Final
build additionally matches case20's depth attachment/depth test/clear state,
and has a 90-second watchdog for the whole controlled probe (joined on finish).
Only final NRO + source ZIP re-upload is in progress; tools session63703.
All five files of the earlier intermediate diagnostic were already SHA256
verified, including the two diagnostic shader assets. Final NRO/source are
verified by SHA256 of uploaded temp then renamed, installed size checked.
Check deployment-render-probe.json for final ID before claiming installation.
Probe trigger render-probe.txt=1 exists. Wine cursor remains21 with no reset.
Final validation record render-probe-validation.json lists five relevant checks,
Switch build and diff-check all passed. No hardware success claim yet.
Once upload finishes, ask launch ONCE and reopen FTP after completion or error;
do not relaunch repeatedly. Read render-probe.log for first failing operation
and each shader variant3/2/1/0 before more full-suite work.

## SHADER-PAIR PROBE + BUFFER/CACHE FIXES — DEPLOYMENT IN PROGRESS

User reports another regular Nintendo error after volume flag fix. Retrieved:
build/wine-d3d9-results/switch-next-validation-error-20261019-212835.
Case20 now gets past dummy_volume init. At first rendering/readback:
dkFenceWait reports queue error info32=13 (GrErrorSwNotify), status65535;
GetErrorInfo fails (no detailed fault data). Test reads black, fails, cleanup and
libnx service-entry checkpoint complete. Cursor21; no pending. This is an actual
GPU fault, not merely CPU loader failure. Don't attribute exact shader cause yet.

Build 40f62506b02060775cf0465b4b80cff419563798 prepared with:
- full 16384-byte vertex-blend UBO even when blending disabled, matching declared
  generic VS uniform range (was only64 bytes/256-byte allocation);
- GPU generation counter invalidates raw and packed shader handle caches across
  GPU recreation; re-uploads and reinitializes code in a new device lifetime;
- named GPU fault log after each synchronized operation;
- optional render-probe.txt trigger runs FOUR shader pairs in ONE NRO process,
  fresh GPU per pair, order simple/simple (3), FFP VS/simple PS (2), simple VS/FFP
  PS (1), full FFP (0). Uses controlled white texture/quad, center readback;
  simple PS should return green, FFP PS white. Sync uploads/clear/draw. Logs
  render-probe.log, writes render-probe-complete.txt after all pairs and removes
  marker. If faulted, abandons COM objects and centrally shuts down GPU instead
  of submitting destructors to a failed queue. Next pair uses fresh generation.
- selection override DIRT2_DEKO_FFP_DIAGNOSTIC used ONLY in probe, normalunset0.
- new assets diagnostic.vert.dksh and diagnostic.frag.dksh.

Switch build passes; FFP state/std140/UAM, GPU paths, shutdown, image creation,
shader cache lifetime ASAN/UBSAN checks pass; diff-check clean. Test count5.
Matching ELF/nm/package generated. FTP upload of 5 files+probe trigger started;
check running tool result/journal deployment-render-probe.json for completion.
Suite cursor/results must stay21 and untouched; no probe results counted as
Wine passes. Game NRO/assets unchanged. No commits. Ask user launch ONCE then
FTP at completion or first error, without re-launching. Need logs before any
further full-suite run. Do not claim root cause/fix verified on hardware yet.

## VOLUME FLAG FIX INSTALLED AND VERIFIED

FTP reopened. Downloaded latest evidence to
build/wine-d3d9-results/switch-before-volume-flags-fix-20261019-212721.
Confirmed validator error is during case20 g16r16_texture_test device startup,
when creating dummy_volume_, BEFORE any test draws; pending20 and next21.
Installed and retrieved-SHA256-verified cf023180ce03781fc0ee897d7592807f48aa3416
NRO and matching source ZIP. Matching ELF/nm + package manifest saved.
Journal: build/package/switch-wine/deployment-volume-image-flags.json.
All completed results preserved. Pending/cursor originals backed up remotely,
and unfinished case20 queued for retry (next20; pending marker renamed).
Three relevant CPU regression checks + Switch validation build + diff-check pass.
Game NRO/shader files unchanged. User should launch once; at first error stop
and reopen FTP. Do not claim all rendering/crashes fixed without hardware run.

## CONCRETE VALIDATION BUG FIXED — FTP REOPEN REQUESTED

User read validator error: DkImageFlags_Usage2DEngine not supported with 3D images.
Confirmed all devices allocated dummy_volume_ with this invalid flag; ordinary
volume allocations inherited it from generic resource flags as well. GpuImage::create
now removes only Usage2DEngine for DkImageType_3D centrally. dummy_volume_ explicitly
requests flags0. 2D/cubemap flags are unchanged. Upload/readback and same-size volume
copies use deko DMA CopyBufferToImage/CopyImage paths, which do not need this flag.
Do NOT claim this proves all black FFP pixels/crashes fixed. Volume AUTOGEN currently
still uses a 2D-engine blit and is a separate remaining limitation to address if hit.

New test tests/d3d9_wine/test_image_creation.py runs actual create method with
installed deko API types and validation constraint, tests volume/depth-one dummy,
2D and cube flag preservation/dimensions/mips; ASAN/UBSAN passed. Switch runner
build succeeded with validation library retained. Texture-storage and GPU-state
regression scripts and packaging were launched (inspect tool result).

FTP connection refused at 192.168.1.148:5000. Async request sent to reopen after
dismissing the error. No upload yet for this fix. User should not relaunch old build.
When FTP available: first retrieve latest results/pending/next/suite/faults, preserve
cursor and results. Validation abort may have advanced cursor before executing test;
record any interrupted case, do not silently count it passed. Upload only new NRO
and matching source with temporary files/hash verification/old backups; package
manifest+matching ELF/nm regenerated by package_wine_d3d9_switch.py. Game NRO and
shader assets unchanged. No commits.

## DRIVER VALIDATION BUILD INSTALLED — FIRST INSTRUMENTED ERROR COLLECTED

User got Nintendo error on first resumed launch. Evidence:
build/wine-d3d9-results/switch-first-instrumented-error-20261019-211943.
Case 19 float_texture failed first pixel (black), then GPU shutdown, SDL quit,
watchdog join, envSetNextLoad success, and userAppExit checkpoint all completed.
No pending.txt, cursor 20. No current ee26... CPU crash report.

Newly available Atmosphere 01792444690_03db12780bd84000.log MATCHES older
comprehensive 15d5b99f build and case16 shademode. Symbolicated matching ELF:
svcBreak -> diagAbortWithResult -> deko RaiseError(result 1) ->
dkQueueSubmitCommands -> Gpu::flush -> GetRenderTargetData -> getPixelColor ->
visual.test_shademode. Return PC 0x13b6f0 needs callsite -4: it is the noreturn
abort branch in dkQueueSubmitCommands, NOT ImageLayout::calcLevelOffset!
Queue state==2 (Error) rejected further submission. Release SDK compiles out
GPU fault warnings and validation; callback cannot record those in that library.
Exact GPU fault/method/address remains unknown. Scratch hypothesis already ruled
out (zero required in new FFP DKSH headers).

Standalone runner now links installed libdeko3dd.a with validation/warnings.
Game still links deko3d release, unchanged. Build:
34ca044c108102c312a5010dc272674dad85fa8f.
Built, packaged with matching symbols and source, uploaded NRO/source and
retrieved SHA256 verified; old files backed up, cursor20/results unchanged.
Journal: build/package/switch-wine/deployment-driver-validation.json.
Verified linked symbols include RaiseError(DkResult,char const*) and WarningMsg.
Ask launch runner once, at first error reopen FTP without repeated relaunches.
Retrieve validation error context/queue notifier in suite.log and fault reports.
Do not claim crashes/rendering fixed. No game assets changed, no commits.

## RELOAD PROBE PASSED — WAITING FOR INSTRUMENTED CASE RUN

User reported return to Homebrew Menu. FTP retrieval confirms ALL THREE reload
stages completed: loader-only, SDL init/quit, GPU init/shutdown. Every next-load
result and loader last-load result was zero. Standard libnx service cleanup was
entered each time. Probe completion file exists, probe marker removed, suite
cursor remains 19. Evidence: build/wine-d3d9-results/switch-reload-probe-20261019-211820.
This rules out an unconditional failure in these empty init/exit paths; it does
not establish that rendering/tests or teardown after rendering are safe.
Installed instrumented build remains ee26bd36b380efd85b823a8ce22bdd52899bc408.
Next normal launch resumes the original 144-case selection from cursor 19.
At the FIRST Nintendo error, user should reopen FTP without relaunching again;
retrieve new suite.log/next/pending/raw faults and matching Atmosphere reports.
Extra checkpoints distinguish SDL quit, watchdog join, next-load setup and libnx
service cleanup. Do not reset progress or claim the crashes fixed.

## RELOAD CRASH DIAGNOSTIC — 2026-10-19

User confirmed the Nintendo “software was closed because an error occurred”
screen requiring title takeover again. These are real process failures. Pause
normal suite runs until the exit/reload cause is isolated.

Latest evidence: build/wine-d3d9-results/switch-before-reload-probe-20261019-211658.
Saved cursor 19: 4 pass, 8 fail, 1 interrupted (case 16 shademode), 6 not-selected.
Case 16 raw fault record has magic 0, so no CPU exception was captured. Earlier
11 cases all logged END + GPU shutdown. Available Atmosphere crash logs do NOT
match candidate 15d5b99f; fatal_reports are old June/July logs. Do not claim a
known root cause or dismiss the user's error as normal restart.

Diagnostic candidate ee26bd36b380efd85b823a8ce22bdd52899bc408 builds successfully.
Only runner/support/fault logging changed. Optional results/reload-probe.txt=0
runs loader-only, SDL, then GPU init/shutdown reloads; no Wine cases or cursor
changes. Completion creates reload-probe-complete.txt and returns to hbmenu.
If it errors, user should reopen FTP immediately, not relaunch again. Check
reload-probe.log, reload-probe.txt, and fault indices 269..271. Additional logs
now distinguish SDL cleanup, watchdog join, envSetNextLoad result, and entry
into libnx service cleanup. Fault file stays open through those stages and is
closed by userAppExit before standard libnx FS service teardown.

Matching ELF/nm and package are saved in build/package/switch-wine. Deployment
record is deployment-reload-probe.json if upload completes. Preserve all test
results and cursor. No game files or shader assets changed. No commits.
Queue scratch hypothesis ruled out: new FFP DKSH headers require zero scratch.
FFP draws are black/stale on hardware; all CPU checks passing did not establish
GPU correctness. Do not claim comprehensive fixes have passed.

## COMPREHENSIVE RUN INTERIM — USER REPORTS MANY CRASHES

Userasksifprogressing. FTPwasreachable; read-onlyretrievalshows next.txt16,
results16inventoryentries:4pass,7fail,5notselected =11selectedcasescompleted.
No pending.txt and no complete.txt. suite.logshowsENDandGPUshutdowncomplete
foreachcase0,3,4,5,6,7,10,12,13,14,15. No recordedcrashentriesyet.
Evidence: build/wine-d3d9-results/switch-comprehensive-interim-20261019-211016.
MostFFPrendercasesnowblack/stalecolor; newMSAAstretchcase3passed151assertions,
newdifferent-sizedclearcase7passed39assertions. CannotclaimbroadGPUfixworking.
Saved11faultbinrecords inspected; initializedrecordsarenotexceptionsunlessmagic
matches57435231. RecentAtmospherecrash_reportsdownloaded12files,butlatestlogs
maystillbeolderthirdbatch; don'tattributewithoutmatchingbuildID.

Askedasyncclarification: Nintendoerror+title takeoverrestart, normalflashes/
blackreloads, orreturntoHomebrewMenueachcase. Awaitingreply. Userwasaskedleave
FTPopenwhilelogscollected. Do notresetprogressoruploadwithoutfindingcause.
No codechangedduringthisstatuscheck. No toolsrunning.

## COMPREHENSIVE BUILD INSTALLED — WAITING FOR FULL HARDWARE RUN

User reopened FTP. Installed and retrieved-SHA256-verified ALL16 files of
combined test build `15d5b99f457fcae4eb4f04a3898dcb8522d036e4` at
`/sdmc:/switch/d3d9-wine` on192.168.1.148:5000.
Deployment journal: build/package/switch-wine/deployment-mac-parity-comprehensive.json.
All old installed files backed up with before-comprehensive suffix; entire
previous results directory renamed to results.before-comprehensive-20261019-210712.
Fresh results/next.txt contains0, verifiedafterinstallation. All144Mac-passing
cases selected. Game NRO/pack/settings unchanged.

Askuserlaunch D3D9 Wine Tests usingtitle takeover, continueafteranyhardcrash
byreopeningrunner, thenFTPwhenitreturnsHomebrewMenu. No furthersmallbatchruns.
HardwareoutcomestillPENDING; lastverifiedparity58/144fromthirdbatch; doNOT
claimthecomprehensivebuildalreadyhas145testpasses(shadersarecompilercounts).
Local13checks pass,145testshaders+897game shaderscompileandSPIRVvalidate.
No toolsrunning. No commits/pushes. Credentialsneverstoredinrepo/docs.

## READY COMPREHENSIVE BUILD — FTP REOPEN REQUIRED

One combined candidate is built and packaged, NOT INSTALLED:
`15d5b99f457fcae4eb4f04a3898dcb8522d036e4`.
Package: build/package/switch-wine/D3D9-Wine-Tests-Switch.zip.
Manifest and matching ELF/nm saved in build/package/switch-wine.
Stage sd/switch/d3d9-wine includes145-shader pack,6FFP/depthutilityDKSHfiles,
license,README,original144selection,sourcearchive. Verifiedhashes+sourcecontents.
Validation record comprehensive-validation.json: all13CPUchecks pass,145/150
shadercandidatescompile+SPIRVvalidate,all897game shaderscompile+SPIRVvalidate.
Game NRO/pack NOT changed or uploaded. Hardwarestill58/144fromthirdbatch.

FTP192.168.1.148:5000 timed out onconnect12sec (authorizednetworktool).
Asyncquestion sent askingreopenFTP; pendingresponse. Do notclaiminstalled.
WhenFTPready, upload thisONEcomprehensivebuild (no moreincrementalbatch), temp
upload/retrieveSHA256verify allchangedfiles, backupoldfiles+entireoldresults,
newresults/next.txt0. Preserveoldremoteevidence. Then askuserfull144run,
reopenafterhardcrashes,FTPafterfinished. No creds inrepo/docs. No commits.

Additional changes after progressnotesbelow:
- Clears bind eachcolor/depthsurfaceindependently soMRTminimumextent cannot
  clip larger clears; extended CPUrecorder checks640x480/200x100+depth.
- ProgrammableVSclippingUBO6+wrapperall6planes; neutralzero fordisabled.
- FFPinverseViewtransposedforcolumnmultiplycliphelper; normalizedlightdir,
  spotlightcosHalftheta/phi; pointmodegating; LightEnable marksFFPdirty.
- Nativeblack-with-alphaONE unbound2D/cube/volume images (typedfallbacks),
  INTZallR andDF16/24 R001 samplingswizzles.
- ResetinvalidatesFFPviewportstate. Rawshaderhashcachepreventsrepeatedloads.
- SPIRVfullbankrelativeindices clamp+zerooutside, DEFoverlays preserved.
- AllsevenMojo patchesverified reproducecompiledsource. Original0001patch
  restoredbyteexacttoHEAD;0002..0007new. .patchedhashupdated.
- build_wine_d3d9_shaders.py structurallyvalidatesifspirv-valavailable and
  refusestranslatorprocesscrashes;stale.spvremoved. Currentpackvalidated145.
- GameLUTworkaround strictnewsourcehashpinned, preservesnewPSepiloguewhile
  replacingonlyoriginalbody. Gamepackbuild/shaders/dx9-comprehensive.
- third_party/deko_ffp/README.md pinsupstreamCC92063..+license.
- Build/testsessionsallcompleted, no toolsrunning.

Knownlimits still needhardware/fullreport; donotclaimall144fixed:
traditionaldepthcompare inprogrammablePSstillnotimplemented;FFPcubeDREF
stillmanualnearest(noPCF);YUVonlyCPU-SCRATCHsupported;manualsRGBfallback
onlyRT0. These aretrueimplementationlimits, notskipsconvertedtopasses.
Thefullrunisforall144togetherratherthanincrementalsubsets.

## COMPREHENSIVE CANDIDATE PROGRESS (latest; supersedes stale implementation notes below)

User wants ONE comprehensive repair build then full144 Mac-passing target run.
HOLD uploads until remainingknown gaps addressed and local validation finished.
No fourth build deployed; installed test remains db80a2257b167bbe6f824dd04283f270c4b747c5.
Hardware parity still58/144. Last evidence switch-mac-batch3-20261019-221515.

Implemented locally after thirdrun, compiles (mostrecentcompile before final few changes):
- Contiguous CPUtexturemip backing fixes crash125, stable nondiscardbufferlocks
  nowwaitGPUinsteadrename. ATIvolume partialblock alignment restored, YUY2/UYVY
  CPU-onlySCRATCH creation+independentblockheight, pitches andoffsets.
- Stateblock typedCapture streamoffset quirk; sRGBviews/samplers.
- FullyintegratedgenericDXVK-derived FFP VS+smooth/flatPS, persistentcachedUBOs,
  16inputaliasing, realtransforms/lights/material/stages/fog/clips/vertexblend.
  Sources originalthird_party/deko_ffp + generator. CPUmath/ABItestadded.
- BasicMSAA(2,4,8), RT/depthcreation/samplemask/rastermode, resolve/blit,
  implicitSwapChain+GetFrontBufferData, Presentfrontsnapshot.
- HiddenAUTOGENmipchain, publicapi_levels1, truefilter+generation-onbind.
- Per-deviceGPUstateowner restores state acrossdevices/helpers. Switchtest
  platform logicalwindowidentities shareoneSDLsurface, allowsmultiDevicecase.
- FeedbackattachedRT/depth snapshot perdraw, persistentstageimages, cachebarriers.
- Correct R/RG float/unorm missingchannels=ONE; V16U16 support; ATI2 G/R swap.
- NativeRGBX/BGRX formats now used forX8RTs (SDKverified renderable), fixing
  destalphablending. PreviousRGBA+swizzleonly violated destalpha.
- PSprogrammable epilogue UBO5: alpha precision16-bitmatchingDXVK, SM1output
  clamp, legacyfogwithZ/W+vertexfog; VSdefaultfog1whenunwritten. Genericmanual
  sRGBencodeforformatswithoutSRGBalias(FFP+programmableRT0). Hardwarealpha
  comparealwaysALWAYS becausefragmentshaderhandlesit.
- Realdepthresolve/sample0 utilityVS+MS/normalFS generated byFFPscript; RESZ
  commandcallsdepthresolve; NVATOCcaps+rasterenable, disablesalphatestwhileATOC.
- Point-modegating, firstprovokingvertex, spotlightcosHalfangles corrected.
- Rawshaderbinaryhashcache avoid repeatedFFPloads leaking8MBcodepool.

Hostcompiler persistentpatches0003legacytextures,0004relativebank,
0005SM1clamp/pointmarker,0006MOVaddressrounding:
TEXCRD(1.1-1.4),TEXLD1.4,_dz/_dw,TEXDEPTH, relativeconstantswithoutCTAB fullbank
withDEFoverlays, undefinedprivateTEMPzero, MOVa0 SM1floor/MOVAnearestEven.
Tool nowexports .spv. Latestprelastchangescompiled145/150 (5remainingmostly
intentionallyinvalid; DEPTH0semanticinputstillrejects, maybepartofvalidcase24).
SPIRVvalidatorcaught mov_test typebug, nowfixed butvalidation mustrerun.
Latesthostoutputbuild/shaders/wine-d3d9-comprehensive uses captured19.
Important OLD build/shaders/wine-d3d9 pack119 is still previous; packagingcopy
must explicitlyuse comprehensiveafterfullvalidation. No uploadyet.

Remainingaudit/work beforecomprehensivecandidate:
- All12CPUtests werepassing beforelatestRESZ/autogen/FFPshadow edits. Current
  testsrunning session19195; hostbuildsession32536. PriorSwitchbuild44314 needs
  polling. Utilityshaderbuild20561 needs polling. No upload sessions.
- NewGPUfeatures needactualcommandmodeltests andcompile. Fixturemocksupdated
  fornewFFPfields, formatblockheight,RESZcommand; don'tweakenassertions.
- Relativefullbank currentlyGPUOOBdynamicindicesnotclamped; needzerooutside
  bankbeforeimageupload? clampSPVaccess andmaskzero.
- CubeShadowFFPmanualnearestcompare is INCOMPLETE (hardwarePCF notimplemented).
  TriedaddingCubeShadow8samplers atsamebindings butUAM32samplercountrejects;
  revertedtoworking32samplers, stillmanualCubecompare. Needcorrectsolution.
- ProgrammablePSdepthcompareonlynormaltextures; needgenericshadowvariantpath
  fornonINTZdepthformats. FFPspecsamplermodes now2forD16/D24, excludesINTZ/DF.
- ManualSRGBfallbackRT0only;MRTsotheroutputsnotconverted. ClearusesSRGBview,
  verifySDKclearconversiononhardware; donotdoubleconvertwithout evidence.
- Shaderlegacy bool/int invalidfixture handling, semanticDEPTH0 mayneedmapping.
- Nondefaultmissingvertexinputs useSDKisFixed, inspecthowconstantdefaultsset.
- NewMSAA/implicitSwapChain/GetFrontBuffer validation/order risks; allocations
  notallchecked, newutilitydepthresolveload usesrawshadercache.
- Needfold/verify persistentpatches applyclean fromupstream. 0005pointmarker
  addedmanually; hunklengthrepaired+1121,15. Update .patched checksumbefore
  build_shader_tools.sh oritwillwipe sourceandredownload (notyetupdatedafter
  lastepilogue? hostonlynotpatch, 0006scriptupdatedchecksum).
- Needbuildall897game shaders forregression (no deploymentgame), spirvvalidate
  allacceptedtestshaders, add meaningfulcommandmodeltests, alldifftestcheck.
- NeedREADMEpinlicenseattributionforthird_party/deko_ffp, accurateHANDOVER.

## ACTIVE STEERING — ONE COMPREHENSIVE BUILD, NO MORE SMALL BATCH UPLOADS

User latest: "Try to fix all issues in one shot and we test them all, instead
of doing it in small batches". Agreed to HOLD next upload, work on ALL remaining
Mac-passing targets incl FFP/MSAA/shadertranslation/resource state, then fulltest.
Do NOT deploy incremental fourth batch or ask user to test unfinished features.
No subagent authorization. No commits. Keep existing game buildunchanged.

Thirdrun retrieved `build/wine-d3d9-results/switch-mac-batch3-20261019-221515/`:
58pass,73fail,12partialskip,1crash,125notselected; all52priorpassesretained.
Crash125 test_mipmap_upload RAWcaptureworksfirsthardware: pc_main_offset756576,
LR757204; addELFmain0x1640 beforeaddr2line =>TextureCore::unlock hashmap lookup,
faultptr0x2222222222222222. OriginalWine testcase writes ENTIREmipchain via
level0Lock. Currentseparatevectorlevel0 allocated4096; testcasefills5460bytes,
corruptheap withlevel1value0x22. Fix underwayhascontiguousCPUtexturebacking
alllevels/faces,managedunlockuploadsallmips. Updated test_texture_storage.py
runs exactchainpattern ASAN/UBSAN and passes. NewCpuLevel layout includesphysical
pitch/slice vsnativeATIapi pitch/slice,CPUvectors singleallocation.

Localafterthirdbatch (NOTdeployed):
- TextureCPUstoragecrashfix above; testpasses. StateBlock typedCapture preserves
initialstreamoffsetlikeDXVK; explicitBeginEndCaptureupdatesoffset. Added
capture_stream_offsets bool +test_stateblocks regression; testsneedrerunlatest.
- sRGB mapping added gpu.h/cpp (linear/sRGB separate descriptorids, viewalias,
releasebothids). SetRenderState SRGB dirtiesRT; flush_targetsusesSRGBformat;
samplerSrgbchoosesSRGBdescriptor. Compilefailedonce slotundefined fixedtoidx,
needsrebuild/test. Colorformat565 sRGBstillrequiresgeneralshader/formatfallback;
SRGBhardwareclearconversionnotvalidated. Currentbinary stillinstalledthirdbuild
becauselatestbuildfailedslot beforefix; nofourthNROclaim.
- `third_party/deko_ffp/` copied3 ORIGINALDXVKGLSLfiles+LICENSE (zlib). New
`tools/scripts/build_deko_ffp_shaders.py` adaptsgenericMacFFPalgorithms todeko
std140bindings,combinedsamplers,oursemanticlocations. BothVSandPS compileUAM!
Outputsbuild/shaders/deko-ffp,VS29952bytes,PS152320bytes. FlatPSvariant newlyadded
scriptnotrerun. NOT integratedbackendyet. NeedcompleteFFPbackendCPUuniformstate
andactiveVS/PSbindings. No needruntimeUAMforFFP;genericshaderhandlesstates.
- OriginalFFPShaderVS18inputslots butUAMfrontendlimit16. Aliasedmutuallyexclusive
tweenPOSITION1/NORMAL1 slots2/3 withmatrixBLENDWEIGHT/BLENDINDICES viaGLSLdefines;
backendmetadata mustselectusagebasedrsVERTEXBLENDmode(255tween).
- FFPoutputsemanticlocationsremappedtexcoord0..7,normal20,color16/17,fog24 tolink
mixedprogrammablestages. GLSLsamplers2D0..7bindings0..7,cube8..15,3D16..23,
2DShadow24..31. Needmetadata binding!=D3Dregseparation ORspecialFFPbindingpath.
CubeShadowcurrentlymanualcompare sampledR (nearestonlyapprox;mustfixfiltering
forfullparitylater). NaNspvN* mappedmin/max/clamp,returnprecisequalifierremoved
becauseUAMrejectsqualifiedreturns;nativealgorithmsotherwiseunchanged.
- FFPUboABIcheckedwithglslangValidator-G-q--reflect-all-block-variables:
VSbinding0 ShaderData2048: WV0,Normal64,InvView128,Projection192,WVP256,
TexMatrices8x64@320,ViewportinverseOffset832,inverseExtent848,Lights8x112@864,
Material80@1760,Ambient1840,Tween1844,DataPrimitives12uintarraystride16@1856.
VSbinding1 SpecConsts80 (20uintcontiguous),binding2 clipplanes96,binding3
WorldViewArray256matrices16384(rowmajor),binding4 RenderStates64(global16@0,
vs.packedPoint@32/36,ffvs.ScaleABC48/52/56). PSbinding0 SharedData768,8stages96:
Constant0,BumpEnvMat[0][0]16,[0][1]32,[1][0]48,[1][1]64,LScale80,LOffset84.
PSbinding1 sameSpec80,PSbinding4 RenderStates48(global16@0,ffps.textureFactor32).
Uniformbufferspersistent withPushConstants cache; doNOTallocate25KBtransient
oneverydraw(120000drawstress). Needrebuildonlyonstatechanges.
- PlanDrawShaderpair {constPackMeta*meta;constDkShader*shader} inDevice,keepvs_/ps_
publicAPIunchanged,draw_vs_/draw_ps_ selectactualvs/psorFFPfallback. Replace
renderhelpersusesvs_->meta/shader withdrawpair,includingprehelperlogsline321.
shaderfilesDKSHloadhelper needed (existingload_entry rawcodeupload canfactor).
FFPclassshouldownmetadataandUBOs,lazilyinitonlyFFPused,nopublicCOMcycle.
- CPUmatrixnativeROW convention: computeWV=World*View,WVP=WV*Projection;
NormalMatrix=invWV (GLSLmultipliesnormalcolumn);InverseView=invView. Store
row_major UBO. DXVKMatrix4 representation differs so don'tcopyoperatororder
blindly. POSITIONT inverseextent{2/W,-2/H,ZEnabled?1:0,1},inverseoffset
{-1-2X/W,1+2Y/H,0,0}. SourcefullyDXVKshadingformulastestedonMac.

Next: integrateFFPfully,thenMSAA,shaderSM1/relative noCTAB,remainingAPIbugs.
Captured19shaderblobs inresults/shaders;18knownrejectedfixtures+1runtime.
Existingbuild_uamcloneunusednotneededforFFPbutmayenablefullruntimecompilerlater.
StillnoFFPrenderingbackendintegrationyet. Keepworkingcomprehensivegoal.

## Latest — 52/144 verified passes; third repair batch installed

Second Mac-targeted hardware run completed without crashes:
`build/wine-d3d9-results/switch-mac-batch2-20261019-220205/`.
Summary/comparison.json: 52pass,80fail,12partial-skip,125not-selected. All42
prior passes retained;10newpasses. No crashes/timeouts/interrupted entries.
Passing-target goal still144;92 remain notpassing. User notified52/144.

Third batch build `db80a2257b167bbe6f824dd04283f270c4b747c5` installed and
retrieved-SHA256 verified for6files (NRO,119shaderpack,shaderreport,README,
sourcezip,reference-pass). Deployment record:
`build/package/switch-wine/deployment-mac-parity-batch3.json`.
Old files and whole results directory preserved remote with before-batch3 suffix;
newresults/next0. Async user request sent to run D3D9 Wine Tests through title
 takeover, reopen after crashes, openFTPafter. No third-run reply yet.
Current local binary/source match installed third build. Game NRO/env/pack
unchanged. Do not claim52 count validates thirdbatch or the game.

New state-block implementation:
- `shims/d3d9/deko/stateblock.{h,cpp}`. Selected entries store saved values plus
  capture/apply closures. Device setters intercept after validation, before any
  mutation/equal-state earlyexit. No primary state changes during recording.
- Constants keyed perregister, so overlapping writes replace only overlapped
  registers. Resource values retain private ownership with qualified concrete
  calls / texture registry, never public vtables. Begin recording block owns
  only a private reference from device; End converts to public parent ownership
  without cycles. Device destruction releases abandoned pending block.
- Pixel/vertex/all masks cross-checked against DXVK CaptureType. Render/depth
  targets and palettes never captured. SetRenderTarget affects primary viewport
  even while recording. MultiplyTransform bypasses recording. Recorded/captured
  NULL declaration does not replace current declaration on Apply (native quirk).
- Pixel shader constants/renderstates/TSS/pixel sampler states vs vertex shader
  constants/renderstates/decl/freq/lights/DMAP separated. All adds textures,
  streams,indices,viewport/scissor,clipplanes,transforms,material.
- platform/switch/switch.cmake and standalone test CMake link new stateblock.cpp.
- Fill missing native defaults for pointsize max, degrees,tessellation.

Further third-batch fixes:
- Clear with nonnullrects,count0 is noop (nullrects,count1 still clears).
- Native texture-to-offscreen and RT-to-offscreen StretchRect validation.
- CPU-pool 2D texture invalid rectangles expose legacy offsets like native;
  integer pointer arithmetic avoids C++ out-of-allocation pointer arithmetic;
  full level snapshots, never invalid regions, go to GPU. DEFAULT staysstrict.
- ATI1/2 volume exposed pitches/slices/raw offsets separated from physical BC
  upload layout; allocate enough backing for either. Core DXT block alignment
  enforced, ATI extension partial blocks allowed like native.
- Includes119-shader pack from previous translator patch work. All897 game
  shaders previously compiled with patch. No game shaderpack deployed.
- Optional DIRT2_DEKO_CAPTURE_SHADERS writes missing bytecode bySHA256; enabled
  only in standalone runner to results/shaders/. Next retrieval must recurse
  that directory. Log from secondrun has19missing keys:18known literal fixtures
  rejectedcompiler,1runtime variant. Build_wine_d3d9_shaders.py --extra-shaders DIR
  accepts captures and retains original bytecode hash; invalid fixtures remain
  explicit failures. Existing output cleanup prevents stale GLSL fakingcompile.

All11 relevant CPU checks passed, actual methods under ASAN/UBSAN whereused:
stateblocks(new),device_state,shader_constant_bounds,texture_binding,
texture_storage,clear_regions,object_refs,pool_lifetime,gpu_shutdown,fault_capture,
adapter(220 original bodies exact). Updated old isolated CPUfixtures include
actual state-entry templates via cpu_stateblock_support.py with recordingoff;
dedicated test_stateblocks.py exercises real recording/Capture/Apply/type masks,
resourceownership/null declarations/overlap/parent life/abandonedrecording.
Switch NRO build and diffcheckpass. No GPUproof yet fornewstateblock feature.

Remaining major work: fixed-function draw shaders entirely absent, MSAA,
SM1shaderinstructions(TEXCRD/TEXDEPTH/SM1.4TEXLD),relative-addressing withoutCTAB,
RT feedback, formats/locking/buffer synchronization,extraSDLwindows. Secondrun
case142 multiwindow stillcannotcreate secondSDLwindow (Switch SDLplatformlimit).
Manyvisualfailures simplyskipdraw whenVS orPSnull. These features require real
implementation; do not lowercaps or change originalassertions tofakeparity.
Nextretrievebatch3 includingresults/shaders, summarize52baseline,target144,
repairremaining. User authorizesFTPdeployment, no subagents/commits/pushes.

## Current status — 42/144 Mac targets pass; second repair batch installed

First Mac-parity hardware run retrieved after user reported no crashes:
`build/wine-d3d9-results/switch-mac-batch1-20261019-215201/`.
Complete inventory269:42pass,91fail,11partial-skip,125not-selected. All144
selected targets correspond to actual pinned Mac passes. No crashes recorded.
Comparison JSON saved there. All144 objective remains active;102 still notpass.
Do not report suite completed as allpassing or count skipped/unselected aspasses.

Second batch build `8c6ec1e4c31d8737a356ae91b4eb42f9f54c3e79` installed and
retrieved-SHA256 verified on192.168.1.148:5000. Record
`build/package/switch-wine/deployment-mac-parity-batch2.json` includes backups.
Previous entire results preserved remote; new results/next0. User async asked to
launch D3D9 Wine Tests through title takeover, reopen after any crash, FTPwhenfinished.
No reply to that second-batch run request yet. Game NRO/pack/env unchanged.

Second-batch implementation:
- TextureCore lazy persistent per-level CPU contents; locks expose full-level
  pitch+offset, preserve untouched regions and previous writes. Each unlock
  copies immutable transient GPU upload snapshot, rather than returning fresh
  transient data with lost contents on every lock. READONLY avoids upload;
  DEFAULT nonexpanded images read actual GPU contents before nondiscard locks.
- Volume locking supports validated boxes, full pitches/slices, persistent bytes,
  perlevel nested lock checks, readonly and default/dynamic readback. Failure
  clears pBits. Native creation validation for dynamic pools/WRITEONLY and core
  DXT base dimensions. Standalone surface nested/double unlock tracking and
  full-image pitch with partial lock offset. ATI scratch surface native exposed
  pitch/offset handled separately from physical compressed layout.
- Full GPU barriers before buffer/image uploads and UpdateTexture overwrites.
- Clear clips enabled scissor and rejects depth/stencil clear without surface;
  Reset clears scene nesting; clip plane oversized indices alias last plane as
  DXVK/native; palette rejects non-FF alpha when alpha palette caps absent.
  Default CLIPPING enabled. StretchRect validates pools/ownership/destination
  usage/filter/depth scene restrictions before GPUcommands.
- Real synchronous EVENT query IssueEND waits GPU completion. Unsupported query
  types return NOTAVAILABLE rather than generic INVALIDCALL.
- New reusable tools/scripts/package_wine_d3d9_switch.py packages matching
  binaries/symbols/source; no credentials in it.
CPU actual-method ASAN/UBSAN checks: test_texture_storage.py(new),
test_device_state.py(updatednativeclip/palettecontracts), test_clear_regions.py,
test_adapter.py allpassed. Switch standalone buildpassed. GPUresults pending.

Local work AFTER installed batch2 (not deployed):
`third_party/patches/mojoshader/0002-spirv-texture-sampling.patch` fixes sampler
swizzle applied to opaque sampler instead of fetched vector in TEXLD/TEXLDL/
TEXLDD; TEXLDD gradients narrowed to vec2 orvec3. Uses actual used_registers
sampler ID (metadata sampler registry has a different iddecl). Identity swizzles
are elided to preserve existing game GLSL/workaround matches. Host compiler
rebuilt. Wine pack in build/shaders/wine-d3d9-batch3 increased117->119 literal
fixtures, allprevious117stillcompile; two previously UAM-rejected shaders now
compile. Final119 fixture rebuild and all897 retail shaders compiled successfully
(session79625 completed). Census:build/shaders/dx9-texture-translation-fix/census.md;
341vertex+556pixel,0failures. These are compilation checks,not GPU rendering proof. Source patch
integrated existing build_shader_tools.sh mechanism and .patched SHA1 updated
for existing ignored source. No game or installed suite shaderpack overwritten.
Build_wine_d3d9_shaders.py now deletes outputs per current fixture before
translation so stale GLSL/DKSH can't hide a new compiler failure.

Remaining major gaps: FFP entirelyabsent, stateblocks absent, MSAA unsupported,
runtime-generated shaders absent from pack, furthertexture/format/state errors.
Do not fake passes by capability reductions or assertion changes. Nextread
secondbatchresults; continue fixingall144. Credentials only in authorized FTP
calls (same user-provided login); do not persist them. No subagents allowed.

## Batch1 deployment confirmed (2026-10-19 21:46:45)

Installed and SHA256 verified all4files for build6a6e111931c75a6f446a85559e650a0980b49e14.
Record build/package/switch-wine/deployment-mac-parity-batch1.json. Oldfullresults
retainedremote results.before-mac-parity-batch1-20261019-214645; newemptyresults/
starts0, reference-pass.txt selects144Macpasses. Gameunchanged. Useraskedasync
tolaunchD3D9WineTestsvia title takeover,reopenafterhardfaults,FTPwhenfinished.
No usertestanswer received yet. We are stillactive onall144 objective. Current
localbinary/source sameasinstalled. Allnewfaultcapture/winflag/refstate changes
firsthardwarevalidationpending. Need retrievefuture results includingfault-*.bin,
compare mac_passing_targets baseline0/144 nowtargetcount, repairremaining.

## Active objective — all 144 Mac-passing cases must pass on Switch

User explicitly requested fixing ALL tests that pass the Mac reference, hoping
this removes game issues. Goal is NOT complete. Do not claim first repair batch
achieves parity. Keep fixing shared causes and require actual Switch results.
No subagents requested/allowed. Existing game NRO/shaderpack/env untouched.

First Mac-parity batch prepared, build6a6e111931c75a6f446a85559e650a0980b49e14.
Source changes this turn:
- New object_refs.h separates application/public COM refs from private device/
  state/cache refs. Public children retain device only while public count>0;
  private bindings survive public count0 without a device cycle. Parent release
  can delete the same child through device destruction; no accesses afterward.
- Stub generator + stubs.inc use ObjectRefs, expose AddRefPrivate/ReleasePrivate/
  SetDeviceParent; generated code must stay synchronized. Device bindings use
  private refs, qualified calls bypass malicious public shader vtables. Existing
  texture registry now has separate private/public callbacks. GetTexture uses
  public, SetTexture/destructor private. Surface texture levels forward private
  ownership to parent texture. Implicit back/depth have public0/private ownership;
  FVF declaration cache private0/public0 semantics and destructor release fixed.
- All resources including texture/shader/declaration retain parent on creation;
  remove VB/IB/standalone-surface manual destructor parent Releases (ObjectRefs
  releases parent on public1->0). Shader constructors moved toshaders.cpp to
  access completeDevice type; Texture successfulinit SetDeviceParent.
- Device destructor submits/waits pendingcommands, clearsbindings/caches, destroys
  dummy, defers UBO/sampler frees then collects all deferred callbacks because
  tests often create/destroy many devices without Present. No premature reuse of
  GPU storage still referenced by unsubmitted commands. Guard partialinitqueue.
- Pool live-byte accounting; GetAvailableTextureMem subtracts actual image/buffer
  allocations from preexisting1GiB estimate, rather than returningconstant.
- Switch test adapter removes SDL_WINDOW_VULKAN (backend is deko); logs actual
  windowcreation error; Mac retains Vulkanflags. Old cases without windowassert
  silently proceededwithNULL because backendignoresnativehandle;23failures had
  explicitwindowcheck. Need hardware confirm newSDLwindowcreation.
- State storage/defaults/readbacks for8TSS,512transforms,material,lights,6clipplanes,
  all float/int/bool shader constants, FVF and streamfreq; matrix multiplication
  uses incoming*stored per Microsoftdocs. Lighting/material defaults populated.
  Scene begin/end pairing tracked. ValidateDevice matches DXVK simplepasses1;
  palettes stored/get/current. Volume-level wrappers added and shared texture
  publicrefs/container/desc. VolumeLock disallows non-dynamicDEFAULT asnative.
- Validate unknown/zero-sized resource creation, rejectSCRATCHbuffers, foreignRT/
  depth/nonusagebinding; UpdateTexture sourceSYSTEMMEM,destinationDEFAULT,
  matchingtype/format/device. Linear pitches now4-byte aligned. Adapter mode
  queries reject formats other thanX8R8G8B8/R5G6B5. GetDevicenulloutputs validated.

Actual-code ASAN/UBSAN tests pass: test_object_refs.py(cycles,implicit surfaces,
resurrection,parentcascade), test_device_state.py(defaults,roundtrips,matrixorder,
scene,etc), test_pool_lifetime.py(actualallocatorreuse/counts/dedicated/failure),
test_texture_binding.py(2D/cube/volume,invalid/null vtables/privatecounts/lifetime),
test_shader_constant_bounds.py(6setters+6getters),test_gpu_shutdown.py,
test_fault_capture.py. test_adapter.py still preserves220 original bodies and
reporter fixtures. Switch standalone build and diffcheck pass. No hardware pass
claims yet. C++methods may expose more failures after earliest oldfailurefixed.

Runner optionalreference-pass.txt selects144 indices (derived exactly from latest
pinnedMacpassrecords). Other125cases statusnot-selected, NEVERpass. No original
Wine assertions/body edits. Freshresults/next0 required. Summarizer now includes
mac_passing_targets count/pass/remaining list; targetgoal144/144, skips count as
notpassing. Pack stagedreference-pass.txt,README,sourcezip,NRO; matchingELF/nm
savedunderbuild/package/switch-wine/symbols/. Core SDK/FFP changes NOTcomplete:
missingfixed-function draws are skipped wheneverVSorPSunset, no stateblocks,
MSAAunsupported,texture/volume locking/persistence missing,format/API cases,
32shaderfixturesrejected. User wants allMacpassing; continue after validation.

FTPinitialauthworkedthenconsolebecameunreachable; asyncaskedreopen, userreplied
FTPready. Upload currently ongoing (session87563), must inspect completion and
build/package/switch-wine/deployment-mac-parity-batch1.json before sayinginstalled.
Script hashesretrievedtemporarybytes, backsoldfiles, installs4files, archives
entireoldresults/ as results.before-mac-parity-batch1-STAMP,createsfreshresults/.
Then ask user tolaunchD3D9WineTestsvia title takeover;hardcrashesreopenuntilcomplete;
openFTPafter. Newfaultcapturehookb1e0 code is included forfirsthardwareverification.
DO NOT modifygamebuilduntilbackendchecks meaningful. Original41game-specific
regressions should also rerun before finalgame deployment.

Independentnextwork: fixed-functionpipeline currentlyabsent; many144targets use
unsetVS/PSsoactualdrawsskipped. Need realFFP shaders/state implementation, not
changingcapabilities/assertions tohidefailures. OfflineWinepack117/149fixtures;
failedreasons CTABrelativeindex4fixtures, TEXCRD/TEXDEPTH unsupportedSM1many,
PSIZE/DEPTH/FOGsemanticoutputs,2UAMsourcetypeerrors. Allretailshader897fixtures
previouslypassedsizechecks; keepgamepipelineunchangedunlessverifiedgeneralfix.

## Full Switch inventory collected (2026-10-19 21:28)

User said tests looked passed, requested check. FTP reachable192.168.1.148:5000.
Retrieved complete results under build/wine-d3d9-results/switch-results-20261019-212847/.
complete.txt says269entriesprocessed, next269, no pending;269uniqueindices,
no missing/malformed JSONlines. Counts:200fail,18partial-skip,50platform-skip,
1interrupted(case186texturevtblcrash),0fullypassingcases.123792successful
individual assertions. Do not equate complete.txt with passing suite. Some cases
hit only final device-refcount checks; many are actualunsupportedAPIs/fixedfunc,
and23fail atwindowcreation (adapter/platformissues need distinguish backend).

comparison.json generated with Mac/DXVKreference:134Switchfailedtests passDXVK;
50bothfailatdifferentfirstline,11bothfailsameline. Largest signature groups:
31refcount/lifetime(26DXVKpass),23windowcreation(15DXVKpass),15unexpectedcolor
(13DXVKpass),13unexpectedhr(4DXVKpass). Signaturegroup != proofsharedrootcause.
One crash remainsknown186; latestAtmoreports are unchangedold0570abcb build
01792437452log and priorreports. User evidently resumedOLDinstalledrunner and
finished remaininginventory after lastturn; pending186 was recordedinterrupted
byoldrunner. Newfaultcollectionb1e0... NEVERinstalled; do not claimnewexception
hook hardwaretested. No need to installcollectionbuild now just tofinishsuite.
Texturefix in source and sharedshaderboundsfix both available for batchnext.
GameNROunchanged by testdeployments. All earlierbatchfix/newhook notes below
are historical work, buildartifactavailable. Currentnextaction is diagnose/fix
sharedcauses inbatches, startinglifetime andwindowadapter then game-relevant
shader/renderAPI discrepancies, withtargetedhardware reruns. No promise full
WineWindows/Exsuitecanpass or allfailurescanbefixedinonechange.

## Latest work — collect entire inventory before batching fixes (2026-10-19)

User steered: isolate crashes, record failures/reasons, finish suite first and
fix shared causes in batches instead of one hardware roundtrip per crash.
Evidence build/wine-d3d9-results/switch-crash-20261019-211356/. Crash186
01792437452_03db12780bd84000.log, build0570abcb..., at SetTexture->set_ref generic
public AddRef. Wine device.c8230 deliberately corrupts texture public vtable.
DXVK native passes this test (10 assertions). Previous constant-bounds fix is
hardware confirmed:178 now completes (then hits existing device-refcount failure).
186 completed entries0..185:156fail,18partial-skip,12platform-skip; crashed186.
Current comparison.json includes manually verified crash186 ->187 accounted,
82 not-run. Failure-signature groups guide batching, not proof of common cause.
Notable group:24 refcount failures,20 passing DXVK. Many other failures are
unsupported fixed function/API or adapter/window issues; do not call all real
D3D backend rendering defects without investigation.

Texture fix implemented: resources.cpp registry maps backend-owned base texture
pointers to core + concrete (qualified, nonvirtual) AddRef/Release callbacks;
register successful2D/cube/volume init, unregister destructor; core_of no longer
calls public GetType; SetTexture/device destructor/GetTexture retain/release
through registry and reject foreign/other-device textures. Does NOT implement
private-vs-public binding refcounts yet. Generic shader bindings still call
virtual AddRef; bogus shader vtables tests currently fail earlier at refcount
checks, so revisit after lifetime fixes. Tests: actual registry + binding methods
with2D/cube/volume, deadbeef/null vtables, repeated bind/get/unbind, final deletion,
foreign/device/stage validation pass ASAN/UBSAN (vptr sanitizer excluded for
intentional vtable mutation). New test test_texture_binding.py. Hardware retest
of texture fix pending; per user collection goal, next run skips recorded186.

New Switch-only switch_fault.cpp exception hook: preopened per-case raw336byte
fault-NNNN.bin, no allocator/stdio/backend mutex in exception handler, saves exact
NRO build ID (read NroHeader), case, phase(test/cleanup), main anchor,PC/LR/FAR/SP,
ESR/error,29GPR+FP; raw FS write flush. Leaves exception unhandled with nonzero
svcReturnFromException to preserve OS/Atmosphere fault path. Best effort only;
if FS/handler fails, existing pending+Atmosphere fallback remains. Watchdog uses
same raw writer before full svcExitProcess. NO unsafe GPU cleanup or NRO reload
after arbitrary CPUfault/heap corruption: manual title-takeover reopen remains
necessary on hard faults/timeouts. On next launch, imports crash/timeout JSON
and proceeds to next case automatically. Normal cases keep clean NRO reloads.
User need not wait for individual crash repairs; reopen runner until complete.

Summarizer now also imports validraw records before console resume, symbolizes
PC/LR with matching savedELF bybuildid via --symbols, preserves earlier assertion
result if cleanupcrashed, groups failure signatures prioritized by DXVK passes.
Test_fault_capture.py tests actual recorder/import using FS/SVC mocks under
ASAN/UBSAN: buildID, CPU context, phase, timeout, unwritten/missing records;
checks raw importer and signature grouping. Not proof of HOS exception-handler
behavior until next actual fault. Switch build passes;220 upstream bodies still
byte-identical; reporter, GPUteardown, shaderbounds and texture tests pass.
New buildb1e0b384c0420e836f6bdf3d3985a6bcbbbe3072; savedELF/nm and package refreshed.
Game NRO/env/shaderpack unchanged. Deployment-fault-capture.json confirms install
if present. Plan: record historical crash186 with verified symbol/build, preserve
prior results, leave next187 and archive pending186; resume at187 for remaining
inventory instead of repairing each one immediately. Do not claim all tests can
pass in one change or that49 unavailable platform entries execute onSwitch.

## Latest work — Wine case178 shader constant overflow crash (2026-10-19)

User reported another crash, left FTP open. Evidence retrieved successfully at
build/wine-d3d9-results/switch-crash-20261019-211145/. Latest report
01792437303_03db12780bd84000.log is NEW buildf6771aa... . Reload fix is hardware
verified:178 completed entries0..177,149fail,17partial-skip,12platform-skip.
No passes at first-failure level (many end with refcount cycles or unsupported
fixed-function behavior). Comparison saved comparison.json in evidence folder.
Do not describe these entries as passing or complete conformance. Pending178,
next179. Test178 device.test_vertex_shader_constant passes UINT_MAX register,1
vector at upstream/device.c7091. PC memcpy, LR Device::SetVertexShaderConstantF
(device.cpp998): reg+count wraps0, allows invalid write. This is a real backend
bounds bug distinct from runner lifecycle.

Fixed macro for all6 setters plus2float getters: reject reg>Limit before checking
count>Limit-reg. Retains existing zero-count/null behavior. Regression script
tests/d3d9_wine/test_shader_constant_bounds.py extracts actual methods and tests
valid first/last/full ranges, UINT_MAX register/count overflow, cross-end ranges,
null and no state/output mutation after invalid input, under ASAN/UBSAN. Passed.
Switch standalone build and diffcheck passed. New build0570abcbeb0dc0f25aef07a4f5d2a4ed73177cac;
matching ELF/nm saved, standalone package and manifest refreshed. Game NRO is
unchanged; same shared source fix will enter its next build.

Deploy NRO,README,sourcezip with verified bytes and timestamp backups; retain
results.jsonl and suite.log. Back up pending178 and next179; set next178 and
remove pending to RETRY crashing case without labelling it interrupted/skipping.
Deployment SUCCESS at20261019-211304: all3files retrieved and SHA256 verified;
progress rewound to178, pending archived, prior178results retained. Record:
build/package/switch-wine/deployment-constant-bounds-fix.json. Hardware retry pending.

## Deployment completed — Wine runner reload fix (2026-10-19 21:08)

After user switched network, normal FTP to192.168.1.148:5000 succeeded.
Retrieved latest evidence under build/wine-d3d9-results/switch-crash-20261019-210748/.
Latest crash01792436112_03db12780bd84000.log confirms OLD build1880b1d9..., same
memset/tmemCreate/nvInitialize fault during reload, now entering case4 lighting_test.
Case2 stretchrect_test failed after159 passing assertions (visual.c4470 expected
invalid-call but got success); case3 partial-skip due unsupported multisampling.

Installed NEW standalone buildf6771aa870245b34e5605af70b17736550143c6c; NRO,README,
source zip uploaded to temporary names, retrieved and SHA256 verified before
backup+rename. Deployment record build/package/switch-wine/deployment-reload-fix.json.
Old files retain .before-reload-fix-20261019-210800 suffix. Remote results preserved
as results.before-reload-fix-20261019-210800; new empty results/ starts at0.
Game NRO/env/shader pack unchanged. User should launch D3D9 Wine Tests via title
takeover, observe reload transition, then open FTP for analysis. Hardware fix
verification is still pending. Prior network-blocked notes below are historical.

## Latest work — standalone Wine runner reload crash (2026-10-19)

User reported crash of standalone runner. Retrieved evidence under
build/wine-d3d9-results/switch-crash-20261019-204044/. Crash report
01792435460_03db12780bd84000.log, build1880b1d9fcb62bfd4bbabe934d1df38f16560f91.
Case0 visual.test_sanity completed FAIL:16 assertions pass, final Device::Release
returns2 (internal backbuffer/auto-depth reference cycle, not fixed here).
Case1 visual.depth_clamp_test crashed BEFORE device initialization completes.
PC memset+0, LR tmemCreate, nvInitialize -> deko Device::initialize -> Gpu::init.
8MiB transfer-memory memset destination was still mapped/inaccessible from prior
load. Runner used _Exit without deko teardown. NRO chainload is SAME Horizon
process: it does not reclaim NV transfer mappings. Disassembly confirms libnx
__appExit does not call nvExit; deko Device destructor does.

Added explicit final Gpu::shutdown: idle queue, destroy swapchain/cmd/queue,
run deferred frees, destroy frame rings/command blocks and all pools (including
abandoned live COM allocations), shader/descriptors/swap blocks, then device
which closes NV services/transfer memory. Called ONLY by standalone runner;
game behavior/build remains unchanged. SDL_Quit follows GPU teardown. Next-load
is armed only after cleanup; watchdog remains active during cleanup and uses
svcExitProcess on timeout (full title termination, manual title-takeover resume)
instead of reloading over live GPU mappings. _Exit retained after explicit cleanup
to avoid static destruction of abandoned COM objects. No claim of COM leak fix.

Validation: Switch standalone build passed; all220 original case bodies preserved
and reporter fixtures pass. tests/d3d9_wine/test_gpu_shutdown.py extracts actual
Pool/Ring/Gpu teardown functions, tests driver dependency order, deferred frees,
all abandoned pool allocations, no-device and repeated shutdown under ASAN/UBSAN.
Hardware retest required. Preserve previous remote results directory before
restarting this run at0, to exercise the formerly crashing0->1 transition.

## Latest work — broad upstream Wine D3D9 runner (2026-10-19)

User explicitly asked to try the entire DX9 suite on Switch. Built a separate
native Wine test runner instead of extending only our41 custom cases. Upstream
sources pinned to Wine840a939ee526137639665ff5f587224d6a36403f under
tests/d3d9_wine/upstream/ (unaltered, COPYING.LIB included). Four modules:
135visual +106device +1stateblock +27D3D9Ex =269entry points. Original source
~2.3MB. No REA skill applied (source work). No subagents spawned.

Generator tools/scripts/prepare_wine_d3d9_tests.py preserves220 runnable original
case bodies EXACTLY, inserts per-case dispatch into START_TEST and conservatively
replaces Win32-only helpers with explicit skip exits.49 entries preclassified
unavailable, including27D3D9Ex (backend does not expose it), Windows desktop/GDI/
window procedures/x86 FPU/assembly/D3D9On12. Ex source retained but module's native
dispatch is empty until extension/platform support exists. No skipped case counts
as passing; do NOT claim all Windows tests execute natively. Helpers in native
adapter implement value-only geometry and SDL window creation; desktop/GDI uses
runtime platform skip, never fabricated successful Windows behavior.

C ABI adaptation: DXVK's native windows_base.h defines THIS/THIS_ empty even inC.
The test-only forced compat header includes windows.h first then redefines those
macros to INTERFACE*This. Native Wine C vtables now match real C++ COM objects.
Mac Wine clear_test passed243 assertions, confirming actual C->DXVK execution.
Original assertion source/line preserved with #line and newline padding. Test
framework uses first-failure per case (longjmp only over C test frames), records
TODO failures rather than hiding them, disables legacy broken() alternatives,
and distinguishes assertion failures / partial capability skips / platform skips.
Failed cases do NOT run later assertions. This is full entry-inventory attempted
coverage, NOT exhaustive assertion coverage or D3D9 certification.

CPU test tests/d3d9_wine/test_adapter.py: all220 case bodies byte-identical;
pass/fail/TODO/skip/first-failure fixtures pass without GPU. Headers/generator
isolated to test target; no upstream code rewritten in-place. All Python scripts
compile, diff-check passes. Optional CMake DIRT2_BUILD_WINE_D3D9_TESTS=ON currently
enabled in build/macos-arm64 and build/switch. Targets d3d9_wine /d3d9_wine_nro.

Complete Mac baseline through current DXVK completed all269 entries:
144pass,66first-assert fail,8partial capability skips,1native process crash,
50platform skips (49preclassified +runtime request). Results +per-case logs at
build/wine-d3d9-results/dxvk/. Native DXVK is not perfect; baseline isn't a spec
oracle. Source TODO tags retained in failures. Crash was case81
visual.multisampled_depth_buffer_test (process SIGABRT), master continued.
Host orchestrator tools/scripts/run_wine_d3d9_tests.py runs each child separately,
90s timeout, resumes completed results. Full native test data is available for
comparison; tools/scripts/summarize_wine_d3d9_tests.py groups Switch-only fails,
same vs different first failure lines, reference-only fails, skips/interruption.
Native self-comparison produces269 accounted entries. Original41 custom tests
remain all41byte-identical on Switch; this broad runner is independent.

Standalone shader pack built by tools/scripts/build_wine_d3d9_shaders.py from
Wine's literal shader arrays through normal dx9shader/MojoShader/SPIRV-Cross/UAM.
149candidates,117compile,32rejected including invalid API-test fixtures AND real
compiler limitations (relative addressing withoutCTAB, unknown registers,UAM).
Do not attribute every missing-shader draw to GPU-state semantics; inspect
shader-report.json. Six nonliteral DWORD arrays are non-shader state/flag/pixel
arrays. Standalone pack is76KB; game-derived pack is NEVER included/replaced.
Paths build/shaders/wine-d3d9/{dirt2.dkshpack,report.json,src,glsl,dksh}.

Found infrastructure defect before running older shaders: deko shader_bytes
assumedSM2/3 instruction lengths; Wine includesSM1. New shared
shims/d3d9/shader_bytecode.h sizes fixed-lengthSM1 operands and variableSM2/3,
comments,PHASE; rejects unknown versions/opcodes/truncation with a bounded helper.
shaders.cpp uses it. ASAN+UBSAN bounded harness /tmp/wine-shader-size-test.cpp
validates all897original game shaders unchanged,149Wine candidates bounded,
all117compiled Wine shaders exact size, invalid/truncated examples rejected.
This source change is in standalone test build; normal game NRO not rebuilt or
uploaded this turn (installed game remains77801dba3f726ee09acea6d5c6684367030ec447).

Standalone console main writes progress BEFORE entering each GPU test, schedules
itself via envSetNextLoad, runs one non-platform case per process, records JSONL,
then _Exit(0) so loader can continue even after assertion failure. Platform skips
can batch without GPU.90s watchdog uses a joined std::thread (NO detach/ENOSYS),
records timeout marker and requests next load. Unfinished pending case becomes
interrupted/timeout on next launch. Real Atmosphere crash can still need user
reopening the runner. All cases processed ->results/complete.txt, exits toHBMenu.
This automatic loader/watchdog flow is built but UNVERIFIED on hardware. Results
persist: don't overwrite or delete them during retrieval. To rerun after completion
archive/rename results directory first. SDK envHasNextLoad false ->manual per-case
resume, explicit log. Supports default argc0/1 and --case INDEX REPORT debug mode.

Standalone build ID1880b1d9fcb62bfd4bbabe934d1df38f16560f91, NRO7,923,909bytes.
Package build/package/switch-wine/D3D9-Wine-Tests-Switch.zip; SD folder
build/package/switch-wine/sd/switch/d3d9-wine/. ELF +nm preserved in
build/package/switch-wine/symbols/<id>.* for crash symbolication. Eight SD files:
NRO, standalone pack, shader report, inventory, README, LGPL, DXVK results,
upstream-tests-and-adapter.zip. Manifest hashes at package/manifest.json.
INSTALLED at192.168.1.148:5000 under/sdmc:/switch/d3d9-wine; all8files
retrieved and SHA256 verified before rename. Deployment confirmed in
build/package/switch-wine/deployment.json (20261019-203544). Game NRO, game pack
and env untouched. First broad-suite hardware run still required.
User needs to closeFTP, launch **D3D9 Wine Tests** (NOTDiRT2) via title takeover,
allow restarts, then openFTP afterHBMenu return. If it crashes, reopenrunner to
resume. Need first hardware run before claiming broad suite results.

Preserve other agent's envmap_list.cpp/hooks/CMake entry. No commits/pushes.

## Latest status — sampler fix VERIFIED on Switch (2026-10-19)

User ran the second test build and reopened FTP. Retrieved full results,
41 BGRA images and log to build/d3d9-tests/switch-20261019-200113-lod-fix/.
ALL41 PASS, zero API/expectation failures. Full comparison against DXVK:
build/d3d9-tests/comparison-lod-fix/comparison.json — ALL41 images byte-identical,
max channel delta0. All four SetLOD-after-binding failures are now green.
This verifies the sampler dirty-state fix, including its actual Switch GPU
behavior, without changing the regression cases or reference images.

Installed build remains77801dba3f726ee09acea6d5c6684367030ec447. Startup tests
are disabled via verified env update after success; backup + manifest in
that result directory (disable-tests.json, env-before-disable.txt, env-normal.txt).
Manual Minus+Plus captures remain enabled. Old failed results retained as well.
Do not claim full D3D9 certification or remaining game-specific artifacts fixed.
Next coverage priorities are MRT/scalar outputs, RT reuse, instancing/packed
vertices and actual game shader reductions. Existing simple native compressed
sampling tests now pass; no evidence from this suite for a blanket BC3 decoder.

## Latest work — first Switch test results and SetLOD fix (2026-10-19)

User ran automated tests and reopened FTP. Retrieved all41 BGRA readbacks,
results.json and dirt2.log to build/d3d9-tests/switch-20261019-195447/.
Comparison build/d3d9-tests/comparison-20261019-195447/comparison.json:
37/41 PASS with all37 passing images BYTE-IDENTICAL to DXVK (max delta0).
Four failures are *_lod_rebind for RGBA8,BC1,BC2,BC3. Every pixel is still mip0
red instead of requested mip2 blue. Single cause isolated: Texture/Cube/Volume
SetLOD update core.lod but don't dirty bound samplers; prepare_draw skips
flush_textures when SetTexture/state has not changed. Native BC sampling,
normal derivative mip selection, alpha, disabled-Z/no-write, depth tests and
multithread upload/render tests all pass. Do not broaden BC workarounds from
these results. Concurrency logging confirms actual serialization in stress test.

Fix: new Device::texture_lod_changed(const TextureCore*) scans all20 pixel/vertex
bindings, dirties kTextures if the core is bound. All three SetLOD methods call
it only when clamped LOD actually changes. Recursive API gate retained. No
shader/texture data rewrite. Existing41-case suite stays byte-for-byte unchanged,
including the four meaningful hardware failures. Switch build/package and diff
check pass; post-fix hardware rerun required. No additional unrelated LOD API
semantics changed. New build77801dba3f726ee09acea6d5c6684367030ec447,
NRO SHA25689fbc807f9b7441716b36c8680d228efdff6898384d0a1e01cef71c39b3c73b8.
Matching ELF/nm saved in symbols/. INSTALLED and SHA256 readback verified for
NRO and env; old remote files backed up. Manifest:
build/package/switch/d3d9-lod-fix-deploy/deployment.json. Old results preserved.
Remote test path sdmc:/switch/dirt2/d3d9-tests/lod-fix-20261019-200113.
User should launch once, close game and reopen FTP to retrieve the rerun.
Do not claim the four tests green until retrieving that rerun, or claim the
remaining game symptoms fixed solely from this isolated backend correction.

## Latest work — systematic D3D9 tests and third capture group (2026-10-19)

User now asks to automate D3D9 correctness against DXVK / existing conformance
suites rather than hunting visual symptoms one by one. Implemented original
backend-independent suite in shims/d3d9/conformance.cpp/.h, integrated opt-in in
bridge CreateDevice before the actual game device. It uses a separate temporary
device, existing built-in SM3 shaders (VS2, PS4/6), ordinary D3D9 API calls and
GetRenderTargetData. No special deko bypass and no shader-pack changes.

41 cases: clears/alpha, ColorFill rectangle, StretchRect subrectangle, solid
shader, scissor, source-alpha blending, alpha rejection, depth ordering, disabled
Z with writes enabled; RGBA8/BC1/BC2/BC3 non-square short-pitch block patterns,
mips0..4, SetLOD after drawing without rebinding; BC2/3 alpha; 24 resource reuse
iterations; three uploading worker threads alongside render/readback iterations.
Known expected colors + tolerance2 asserted independently of DXVK. Results JSON
and each64x64 BGRA image are saved; failures also logged; opt-in environment:
DIRT2_D3D9_TESTS=sdmc:/switch/dirt2/d3d9-tests/v1
Normal package env remains comment-only (tests off); enable this in uploaded env
for the console test, retrieve results, then remove it for normal play. Each
launch reruns/replaces that output directory: use unique paths to retain runs.

Mac standalone reference runner tests/d3d9/main.cpp, optional CMake target
DIRT2_BUILD_D3D9_TESTS=ON / d3d9_conformance (build/macos-arm64 configured ON).
No game/assets needed. Uses same statically linked DXVK/MoltenVK/SDL functions
as game; must export all WSI functions and set DXVK_WSI_DRIVER=SDL3. Mac GUI/GPU
execution requires unsandboxed exec permission (auto-review approved). Actual
reference runs build/d3d9-tests/dxvk and dxvk-repeat pass41/41 on Apple M1 Max,
DXVK3.1.1-dirt2.1; all image bytes match between runs (tolerance0). DXVK reports
spoofed AMD adapter in D3D9 identifier; hardware identity comes from MoltenVK log.

Dependency-free tools/scripts/compare_d3d9_tests.py compares reports/images,
returns failure for API/expected-pixel failures, missing cases/images or pixel
mismatches; writes comparison.json and RGB amplified-diff/candidate PNGs.
Tests: reference/self and repeat runs pass; mutated pixel, missing image and
missing case detected. docs/D3D9_TESTS.md has usage, limits and expansion plan.
Wine D3D9 visual/device/stateblock tests exist, but Windows framework/windowing
need adapting for Switch. Their LGPL must be retained if copying cases. Our
suite is original, NOT a Wine port or full D3D9 certification. DXMT is D3D10/11;
DXVK is the direct DX9 reference. Add MRT/VPOS/RT reuse/instancing/packed inputs/
sRGB/dynamic-buffer/partial-BC tests next. Current manual dumps are not complete
API traces, so cannot yet replay whole frames without a new trace/replay layer.

New build61518824b6070baf0ce4e7336710e95969af7da8 includes suite + all changes
below + previous0.5x/720p resolution fix. Switch build/package + diff-check pass.
NRO SHA256893b3ec653522dcbdb4af4e7a1cd7dd7492ce06d152e1e18a42fc4de1c82c621.
Pack unchangedSHA256cb34d9dbec8dbc98005496eb015d1e70572ffdc3e69c9f1b261abc7e3bfbe3aa.
ELF/nm saved under build/package/switch/symbols/<buildid>.*.
INSTALLED after user reopened FTP at192.168.1.148:5000. NRO and env uploaded
via temporary files, retrieved and SHA256 verified, previous files backed up,
then renamed into place. Remote shader pack hash also verified unchanged.
Deployment record: build/package/switch/d3d9-conformance-deploy/deployment.json;
old/new env preserved there. Remote test path:
sdmc:/switch/dirt2/d3d9-tests/v1-20261019-195447
Backups: dirt2.nro.before-conformance-20261019-195447 and
         env.txt.before-conformance-20261019-195447.
User must launch once, then close game/reopen FTP to retrieve results. Tests run
automatically before normal game startup; no manual capture needed. Remove
DIRT2_D3D9_TESTS from remote env after collecting this run. No console results yet.

Third manual group evidence build/package/switch/manual-evidence-3/ contains9
captures at frames7085,7223,7422,7733,8511,11526,11601,11815,14447; root/per-frame
logs and all numbered targets downloaded. Last group's menu-depth zero mosaics
and blue race cast are absent now; do NOT claim other listed symptoms solved.
New livery's base texture8 (256x256 DXT1,9mips) is a clean green car photo, yet
rendering has rainbow stripes. Earlier captures lack lower mips. Leading-level
interpretation corrected: user wants01; white0 is visible left,1 missing right.
Shadow/outline draws788/789 (frame7733) only tell part of story; opaque fill uses
an earlier shader/geometry not captured by old filter. Digit textures have4mips,
so earlier single-mip BC3 expansion cannot fix those. No new speculative shader
rewrite or broader BC3 expansion added this round.

Definite source defect fixed: Switch backend ignored D3DCREATE_MULTITHREADED
although bridge already forces it (game loader/render threads share device).
GPU command recording, transient/pool allocators, descriptors and device state
had no serialization, unlike DXVK. New global recursive ApiLock in gpu.h/.cpp,
covering backend device/resource/shader/D3D9 APIs including generated stubs and
IUnknown QI/AddRef/Release, so direct D3DX calls are covered too. First8 contended
entries log serialized concurrent method/thread. Generator updated; static
refcounts remain atomic. Private nested API calls/releases require recursion.
Actual ApiLock extracted host ASAN test with4threads/80,000 vector writes,
recursive lock and active-call count passes. Switch builds. Hardware relevance
to the flickering symptoms is still UNVERIFIED; do not promise it fixed all.

Further manual diagnostics now capture all draw geometry (old shader-family
filter removed), all texture mips and cube/layers up to total16MiB per texture
(volume slice0), batching one submission per texture. Explicit budget omissions.
Base filenames unchanged; extra texNNN_mipMM_layerLL_WxH_fmtF.raw. Readback arena
pre/post CPU cache flush; Full barrier before batched copies. Capture identity
uses GPU image address instead of recyclable CPU object pointer. Manifest
updated. Geometry buffers remain content-deduplicated, max8MiB each, omissions
logged. PS sampled textures only. All this remains manual Minus+Plus, no timed
capture enabled. Latest build not tested on console yet.

Known review candidates deliberately not changed before hardware suite:
Texture/Cube/Volume SetLOD update core.lod without dirtying bound samplers;
source suggests lod_rebind may fail. Depth write with depth test disabled may
also need gating depending on native deko semantics; suite tests it directly.
Do not assume either explains game symptoms without evidence. Readback cache
reuse was considered but no speculative runtime coherency patch made.

Preserve other agent's envmap_list.cpp, hooks.txt and CMake entry. No commits.

## Latest work — Switch 0.5x / 720p baseline (2026-10-19)

User requests 0.5x resolution and correct1x=720p on Switch (SDL had reported
1080p as1x). New Switch build ready, NOT installed yet: FTP192.168.1.148:5000
refused connection. Asked user to close game/reopen FTP. Upload NRO only once
available; existing shader pack is unchanged from the previous graphics build.

Switch display_modes/display_size now use stable1280x720 as the render-scale
baseline instead of SDL desktop-mode/density (which can retain docked1080p).
This baseline is stable in both modes:1x=1280x720,0.5x=640x360,0.75x=960x544
(aligned to8 as before),1.5x=1920x1080. Saved resolutions already in the new
list remain at their existing pixel size; unsupported saved sizes normalize
through the existing startup logic. User still chooses0.5x in the menu.

Found another necessary blocker: game's mode-list builder atEC879B rejects
heights below480 even when adapter enumeration offers them. Added a Switch-only
CMake source-copy step: tools/scripts/switch_resolution_source.py finds the
single generated shard containing that exact instruction, validates its expected
comparison, and substitutes minimum360 in a build-directory copy. All four
lifted copies of this check are covered. Original executable and shared generated
source stay untouched; Apple builds retain480. Relifting/renumbering is handled
by finding the shard by instruction marker at CMake configure. Unexpected source
fails configuration/build instead of silently losing the option.

Tests: actual Switch mode-generation/display-size source passes a host harness:
640x360 at0.5x,1280x720 at1x, all6 modes multiples of8 and within new minimum.
Patch test confirms exactly4 comparisons changed and all other bytes preserved;
changed/missing instruction cases rejected. Switch build/package and diff-check
pass. Static archive contains resolution.o and no old gen_0398.o.
Build IDd42622584f90190cc8570c8d9460d2fca449d969; save matching symbols before
upload (see build/package/switch/symbols/). Shader packSHA256 stays
cb34d9dbec8dbc98005496eb015d1e70572ffdc3e69c9f1b261abc7e3bfbe3aa.
Hardware menu selection/capture at640x360 is still unverified.

## Latest work — second manual capture group / surface + HUD fixes (2026-10-19)

Retrieved eight new captures from 192.168.1.148, frames2711,2980,7096,10431,
10625,11802,15233,16349, under build/package/switch/manual-evidence-2/.
All original target BMP/raw files, screenshots and root log preserved; selected
HUD/material textures and constants also downloaded. Source BC3 UI alpha is
intact; PIAA barrier source diffuse is correct. User reports table artifacts,
FREE RACE SETUP artifacts/missing level digit, barriers resembling monitors,
and clipped progress markers. User confirmed badge must read01. Later close crops show the white0 at left
and the1 missing at right; the earlier leading0 interpretation was mistaken. Capture says4,100XP to Level2; do not
alter player/save data.

Positive hardware evidence: previous PS72af depth fix works (zero zeros in
frames2711/2980 and both races); previous final race color rewrite works (race
RGB means balanced, no blue cast). New menu noise uses two other R32F MRT1
variants: PS1c85f5d10de8 / SHA1 cd02b1bc995caefdc3576bfac6b5abd89d413b1c,
and PSc911a1ab31c8 / SHA1 9a4107bdcd78912caf8b7c45bcde012836214ddb.
Frame10625 has81,195/522,240 zero depth pixels,11802 has112,185. Extended the
same exact-ID replicated-depth workaround to these two (five IDs now total).

Found a separate definite compatibility mismatch: 183 pixel shaders use VPOS
but dx9shader passed gl_FragCoord directly. D3D9 integer pixel centers need
xy-0.5; local DXVK SM3 sm3_io_map.cpp implements this same subtraction.
Added lowering to dx9shader (preserves z/w), rebuilt host translator, recompiled
only183 affected shaders +2 new depth variants. Kept the other712 compiled
entries exactly. All897 interfaces/metadata unchanged. This may improve surface
lighting alignment/edges; it does NOT prove the table/barrier symptoms resolved.
Need fresh console captures to evaluate these surfaces; old dumps lack geometry
and lower texture mips. Do not call monitor-like barriers solved yet.

Candidate HUD fix: extend prior BC2->RGBA8 upload path to single-mip 2D BC3/DXT5
textures, preserving compressed API locks. The level-digit atlas and marker
source alpha are intact; native compressed sampling is suspected from similar
BC2 alert failure but has NOT been isolated in an A/B. Scene mip chains stay
compressed. DIRT2_DEKO_EXPAND_BC3_UI=0 opts out (BC2 opt-out unchanged).
GpuImage.expanded_bc=0/2/3 identifies storage mode. Actual upload code tested
under ASAN against independent BC3 reference on all downloaded HUD textures,
both alpha interpolation branches and partial BC2/BC3 blocks; alpha exact,
RGB at most1byte different due reference565 expansion. Hardware test required.

Capture additions: draw geometry for UI and affected surface families is saved
under geometry/: drawNNNN.txt includes declaration, stream offsets/strides/
frequencies, index bindings, viewport/scissor/cull, draw range and shaders;
content-addressed raw buffers deduplicate content, cap8MiB per buffer (explicit
omission otherwise). UP draw bytes retained. Only manual captures invoke this.
Sampled texture capture still records mip0/layer0 only; cannot assess bad lower
mips from existing evidence. If monitors persist, add mip-chain capture.

Screenshot copy now works on hardware. Log copy still fails opening the active
writable log on HOS. Changed Switch stderr to w+ and snapshots to read that same
handle under flockfile, restore its file position, preserve concurrent logs.
Host ASAN test passes with >16KiB data, concurrent writer, destination failure,
continued logging and exact copy. New console validation required.

Build IDbd1d7f63c73e4816f1ab7224cdd80503db9af0dc, matching ELF/symbols saved.
Switch build and package pass; packSHA256
cb34d9dbec8dbc98005496eb015d1e70572ffdc3e69c9f1b261abc7e3bfbe3aa.
Exactly185 code changes, all897 metadata entries unchanged. Previous pack saved
as manual-evidence-2/shaders-before-fixes.dkshpack. NRO + shader pack installed
and SHA256-verified. Remote backups use .before-surface-hud-fixes-20261019-190336
suffix; details in manual-evidence-2/deployment.json. Hardware test pending.
Zcull enabled, no timed capture. Preserve other agent's envmap hook/CMake/config.

## Latest deployment — additional menu depth and race color fixes (2026-10-19)

Console IP changed to 192.168.1.148 (port5000, same credentials; do not put them
in this document). Retrieved seven manual captures, all original target BMP/raw
files and root log under build/package/switch/manual-evidence/. Captures span
frames2224,2367,2593,3358,3472,7044,7529. User reached and drove a race, so the
previous race-loading mapper change now has positive hardware coverage.

Findings and fixes installed:
- Paper menus have the same zero-depth mosaic as the previous main-menu bug:
  frame2367 has723,968/921,600 zeros;3358 has748,278;3472 has733,149.
  Their R32F MRT1 uses the still-unpatched alpha-tested PS72af51316f14,
  source SHA1 b54d7409f49fe5db26ec292201b01c6d6a2eae1c. Extended the existing
  exact-identity depth-output workaround to it, preserving alpha discard and
  motion outputs. Other unobserved shader variants were not changed.
- Racing PS746a4f246a6a (SHA1 3319a986c1ccd891e116ec8160ef9feffc621f01)
  corrupts only blue in the final LUT pass. CPU evaluation using captured scene,
  bloom, LUT, sampler state and draw331 constants agrees in R/G within mean
  absolute error0.00016; blue expected mean0.422605 vs GPU0.999971 in center.
  Re-expressed the exact LUT/matrix/bloom/vignette arithmetic with explicit RGB
  locals instead of masked register reconstruction. No color tuning or effects
  removed. Source hash guard rejects changed translations. Exact codegen defect
  remains unidentified; console verification of the rewrite is required.
- All seven captures' screen.bmp and dirt2.log aliases were empty: copy_file
  returns EIO on Switch. Original targets/readbacks are valid. Replaced both
  aliases with buffered FILE copying, checked read/write/close errors. Also fixed
  Switch RGB10A2 target previews to use native channel order (raw unchanged).

Validation: Switch build passes; both changed shaders compile from fresh
translator output. 10,000 random matrix/bloom/vignette equivalence cases pass.
Actual buffered-copy helper passes ASAN host tests for binary data >buffer size,
empty/truncated outputs, missing input and invalid destination. Pack comparison
confirms exactly2 changed entries and895 original code/metadata entries retained.
Pack SHA256 d530e96db997a6ae012654bf7202a217b3a7726c82f9b4084f74bc7688e0e595.
Previous tested pack preserved as manual-evidence/shaders-before-20261019-fixes.dkshpack.
Build ID20e12a7f9c01cdc9c2342736ba2f3405a747c46c; matching ELF/symbol map saved
in build/package/switch/symbols/. Standard zip rebuilt. Capture analysis script,
expected race colors and menu zero-depth visualization saved with evidence.

NRO and shader pack installed and SHA256-verified over FTP. Remote predecessors
retained with .before-menu-race-fixes-20261019-182634 suffix.
Normal env unchanged; Zcull enabled, no timed captures. Must verify noisy options/
car selection menus and race colors on hardware, then Minus+Plus captures again.
These new shader changes are NOT yet hardware-confirmed.

## Latest deployment — hotkey dumps + race-loading mapper change installed (2026-10-19)

Installed and SHA256-verified current NRO on Switch via user-authorized FTP.
Remote predecessor has .before-manual-dumps timestamped backup. Normal env remains
comment-only; fixed shader pack unchanged. Build IDd286ada282dccd8b2e47e9f22f491ce6e25749b8.
Matching ELF + symbol map preserved in build/package/switch/symbols/.

Minus+Plus together requests the next complete frame. Release both to rearm;
short rumble=start, longer rumble=complete. One unique folder per capture under
sdmc:/switch/dirt2/gpu-dumps/, including screen, raw targets/HDR, sampled textures,
constants, state/shader metadata and log. Capturing stalls while saving. Requests
while busy ignored; no time-based capture enabled. Hardware hotkey test pending.

Race-loading crash root is guest-memory mapping ResourceExhausted0xCE01 in log,
with secondary HID abort on teardown. Mapper now uses larger aligned backing
and a fixed reserved alias window to reduce resource pressure. Build and ASAN
mock-kernel tests pass; MUST retry race loading on hardware before calling this
resolved. The new report will identify exact failing SVC if pressure persists.
Name-entry HOS keyboard fix was hardware-confirmed by the same run's game log.

Detailed manual-capture format, tests and crash evidence are in the final section.
Earlier deployment/waiting-for-FTP notes below are historical.

## Latest deployment — HOS keyboard crash fix installed (2026-10-19)

Retrieved Atmosphere crash report01792419903 and identified primary failure:
Switch pthread_detach returns ENOSYS; XShowKeyboardUI's detached std::thread
throws/terminates, tearing down HID while SDL still polls. HID abort is secondary.
Switch request now queues the native HOS keyboard directly on SDL's main thread
and completes the guest overlap/event on return; no detached keyboard thread.

Installed and SHA256-verified new NRO over authorized FTP. Old executable has
.before-keyboard-fix timestamped backup. Build ID6b53e3ca81fb12eb4e9c3d597da9da1ef05f25f5.
Normal shader pack/env retained (Zcull enabled, both graphics fixes, no captures).
Standard package rebuilt and checked against executable. Request-path ASAN tests
and Switch compilation passed; user must now verify name confirmation/cancellation
on hardware. Detailed evidence and implementation notes are in the final section.

## Latest deployment — debug cleanup installed (2026-10-19)

The user reopened FTP. Installed and SHA256-verified the current normal NRO,
normal shader pack, and comment-only env.txt. Each prior remote file has a
.before-debug-cleanup timestamped backup. Zcull is ENABLED. Both graphics fixes
are enabled by defaults; automatic captures, sync probes and verbose tracing
are off. This cleanup build awaits user smoke test; do not claim it hardware-tested.

The preceding BC2 A/B's normal-colour alert capture was retrieved to
build/package/switch/evidence-ui-bc2-clean/ (log, old env, final BMP+raw).
Visually verified smooth panel borders and continuous green selection bar.
Older status paragraphs below about waiting for FTP/install are superseded.

## Latest status — torn alert fixed on hardware (2026-10-19)

User reports CREATE AUTOSAVE is clear with DIRT2_DEKO_EXPAND_BC2=1, while game
is still running. Persistent fix now enables BC2->RGBA8 expansion by default for
static DXT2/3 textures on Switch. DIRT2_DEKO_EXPAND_BC2=0 opts out. Native sampled
alpha produced the comb, while decoded source alpha, vertex alpha and geometry
were correct in the discriminator. Exact driver/hardware/compiler cause remains
unresolved. The fix preserves source texture transparency and original shaders.
GPU memory for these textures grows4x; upload does CPU decompression.

The console already has the verified working A/B NRO with expansion enabled in
env.txt, plus the NORMAL shader pack (retains earlier depth fix). The only source
change after user verification is default enablement/opt-out, same enabled path.
Normal production package is rebuilt from current source. No commits/pushes.
Waiting for user to close game and reopen FTP to retrieve the clean final capture
and replace env.txt with EXPAND_BC2=1 only (retain explicit1 for installed A/B NRO;
remove FRAME_DUMP so normal play stops automatically capturing). Do not change
the working shaders. User already authorized this FTP cleanup.

## Latest status — main-menu noise fixed on hardware (2026-10-19)

The user reports the menu is clean with the replicated-depth shader workaround.
The retrieved capture (`build/package/switch/evidence-replicated-depth-1300/`)
contains 2,073,600 valid R32F depth values, zero zeros, range
0.1807837486–0.3264535964. Original capture had 1,520,511 zero pixels.
The original motion-vector arithmetic, alpha discard, and depth testing are retained.

The persistent correction is `fix_depth_mrt_output` in
`tools/scripts/dx9_shader_census.py`: match exactly two game bytecode identities,
replace their MRT1 depth output with replicated depth, and reject unexpected
translated source. These shaders' MRT1 is R32F, so unused GBA changes do not alter
the stored value. Do not generalize this to arbitrary multichannel outputs.
This isolates the observed failure to the compiled depth-output expression;
the exact UAM/Maxwell compiler defect has NOT been established.

All 897 shaders compile. A fresh full rebuild is byte-identical to the tested pack
(SHA256 e53e9d090ee307237d1f4e20c53d63c0c517a7336b5034becb1fade033410f43).
The standard shader output and Switch package now include the correction.
The working pack stays installed on Switch; temporary automatic captures are disabled.
All evidence and remote backups are retained. Only the main menu has been verified;
race rendering still needs normal gameplay coverage. Earlier hypotheses below are
historical and are superseded by the hardware evidence in the final sections.

# Handover: Switch D3D9-on-deko3d, frontend noise

Date: 2026-10-19. Branch `main`. Nothing from b90ba49 onwards has been pushed: the user pushes.

## State

- The frontend renders on the Switch with correct colours and no luminance fade. It runs at about 19.5 fps, with 617 draws per frame.
- **Open bug:** noise across the frontend image.
- Recent commits, all unpushed:
  - `759186a` deko3d: queue without Zcull. This did **not** fix the noise; see below.
  - `01e1233` Frame dump: raw texels of full-size targets (`.raw`); deko3d logs the frame's MRT draw states.
  - `5916f2a` Shader pack: vertex shader outputs start at zero.
  - `4c188f6` deko3d: mask colour targets the pixel shader does not write.
  - `4db6f4a` D3D9 bridge: renderer-independent frame dump (`DIRT2_FRAME_DUMP=<frame>`).
  - `c8d5caa` deko3d: barriers between rendering, copies and sampling.
- Do not commit the other session's files: `shims/CMakeLists.txt`, `tools/recomp/config/hooks.txt` and `shims/misc/envmap_list.cpp`.

## What the noise is

The frame is 1300 (`DIRT2_FRAME_DUMP=1300` in `sdmc:/switch/dirt2/env.txt`). The dump is in `build/package/switch/framedump/`, and the log is `build/package/switch/dirt2.log`.

Pass 01 draws into two targets, with 208 draws:
- RT0 is A8R8G8B8 and holds encoded normals.
- RT1 is R32F and holds view depth.

The focus pass (dump 05, `s0=#2`) reads RT1. Because RT1 is wrong, the focus pass treats the frame as out of focus (R 0.75, G 1.0, against 0.083 on the Mac), and the composite comes out noisy.

RT1 on the Switch (`02_1920x1080_fmt114.raw`):
- Where it is written, values are correct and smooth, for example 0.226. The maximum is 0.326, the same as on the Mac.
- **72.8% of pixels are never written.** They are 0, the clear value. They fall in **4×4-pixel blocks**, at random over the geometry.
- Before `5916f2a` (VS outputs zeroed), the holes held `0xFF7182xx`, which is RT0's colour bits with alpha 0xFF. That history may be a clue.
- On the Mac (DXVK), the RT1 mean is 0.245 (min 0.186), with no holes.

RT0 is written across the whole screen, but its value is nearly uniform (`0x718200`), so holes can't be seen in it.

## MRT draw states logged in pass 01

| PS | Outputs | Blend | Write masks | Z test / write / func | Formats |
|---|---|---|---|---|---|
| `e6b631a7163b` | 3 | 0 | F F | 1 / 1 / LESSEQUAL | 21 / 114 |
| `e80dc3fc05c7` | 3 | 0 | F F | 1 / 1 / LESSEQUAL | 21 / 114 |
| `0dd3a9fe6a08` (full-screen quad) | 1 | 0 | 0 F | 1 / 0 / LESSEQUAL | 21 / 114 |

The full-screen quad writes RT0 only. RT1 is masked off by `4c188f6`.

Blending and alpha test are off, so the blend theory is ruled out. Zcull is ruled out too: the holes are unchanged with it disabled.

`e80dc3`:
- source: `build/shaders/dx9/glsl/982fa4f7d93a58985e5b3e06bd7e62dc7485dd14.frag.glsl`;
- it `discard`s when `tex.a * v2.w - 0.4 < 0`;
- it writes `oC1 = v3.z`, where `v3` is TEXCOORD4 at location 4;
- its VS (`9a430b…`, `7a0d8b…`) writes location 4 `.z`, so the interface matches.

Pack names are the SHA-256 prefix of the d3dbc. Map them to the sha1 file names with:

```python
import hashlib, os
for f in os.listdir('src'):
    print(hashlib.sha256(open('src/' + f, 'rb').read()).hexdigest()[:12], f)
```

Run this in `build/shaders/dx9`. The equivalent shell loop is too slow.

## Leading hypothesis (not yet tested)

The holes are draws **failing the depth test** against a depth buffer that is wrong in 4×4 blocks. The full-screen quad writes RT0 everywhere without depth writes, so RT0 looks complete while RT1 has holes. Things to check:

1. **Dump the depth buffer** at the start of pass 01 and at the end of the frame. Use `read_image` on the depth image (Z24S8 is 4 bytes per texel) and write it raw, as for RT1. Check whether the 4×4 blocks are there.
2. **What is the depth surface?** It might be a depth texture (INTZ/DF24-style) that another pass uses or samples, or it might share memory with something else. Log `CreateDepthStencilSurface`/`CreateTexture` with depth usage, and `SetDepthStencilSurface` during frame 1300.
3. **Quick A/B:** an env toggle that forces `depthTestEnable = false` for draws with `targets_[1]` bound. If the holes disappear, the depth buffer is confirmed as the cause.
4. **Depth clear** (`Device::Clear`, `device.cpp:650`): check that the game clears Z before pass 01, that the clear's scissor covers the full target, and that `ZENABLE` = `D3DZB_TRUE`.
5. **Other 4×4-granularity suspects** if depth is clean: the depth image layout or flags (`image_flags` in `resources.cpp:225`), and the depth view bound in `flush_targets` against the colour targets' size.

## Diagnostics available

| Variable | Effect |
|---|---|
| `DIRT2_FRAME_DUMP=<frame>` | Bridge. Writes BMPs of every render target, `.raw` for full-size R32F/A8R8G8B8/X8R8G8B8, a log line per target, and the MRT draw-state log (deko). |
| `DIRT2_DEKO_DUMP=<frame>` | deko-only target dump. |
| `DIRT2_DEKO_SYNC` | Waits after every draw. |
| `DIRT2_DEKO_SELFTEST` | Runs the self-test. |

Crash symbolising: ELF `main` is at `0xee30`.

```bash
printf 0x%x $((0xee30+off)) | xargs aarch64-none-elf-addr2line -f -C -e build/switch/dirt2.elf
```

Build and package:

```bash
cmake --build build/switch && bash tools/scripts/package_switch.sh
```

This produces `build/package/switch/sd/switch/dirt2/dirt2.nro`. The user copies files to the Switch by hand: never use their FTP credentials.

## After the noise

- **Performance:** trim the conservative barriers. The full barrier on every copy, and `DkBarrier_Fragments` on every target change, are both suspects.
- **Zcull:** decide whether to bring it back with `DkInvalidateFlags_Zcull` on depth target changes, now that it is known not to be the cause of the noise.

## Codex investigation (2026-10-19, awaiting hardware trace)

The source of the holes is **not established**. No new graphical fix is claimed.
The current log and raw files confirm 72.8265% zero R32F pixels. However, RT0
contains the same valid motion-vector values in both the zero and nonzero R32F
regions. These shaders encode motion vectors, not normals. The final fullscreen
shader has RT0's write mask zero and, with the current shader-output mask, RT1's
write mask zero too. It cannot explain complete RT0 coverage by filling RT0.
Thus the prior inference that missing depth-test coverage causes the holes is
not supported by that argument. Zero pixels in an end-of-frame dump also do not
prove those pixels were never written.

Checked: packaged shader bytes match build/shaders/dx9/dksh; UAM TGSI preserves
GENERIC[0] and GENERIC[4] between the relevant VS/PS pairs; compiled fragment
headers export both targets (0xFF), with 8 output registers. Image storage size
and alignment follow deko3d's layout API. Zcull remains disabled, but its source
comment now explicitly says disabling it did not establish the cause.

Added opt-in diagnostics (Switch only):
- `DIRT2_DEKO_MRT_PROBE=1300`: capture a 64x64 region at (96,96) of RT0, RT1,
  and hardware Z after each MRT draw; log shader pairs, primitive counts, image
  addresses, allocation offsets, raw hashes, zero counts, and R32F statistics.
  Full-size raw captures are made after MRT clears, before the shader that does
  not write RT1, and when MRT targets are unbound. Files go to `mrt-probe/` next
  to `dirt2.log`. Raw rows are tightly packed; dimensions and D3D format are in
  each filename. Capture synchronizes the GPU; if it makes noise disappear,
  investigate a hazard rather than interpreting the trace as normal timing.
- `DIRT2_DEKO_MRT_NO_DEPTH=1`: optional A/B only; disables depth testing for
  MRT draws. It is NOT a fix and is not enabled in the first diagnostic run.

Next: run the diagnostic NRO with `DIRT2_FRAME_DUMP=1300` and
`DIRT2_DEKO_MRT_PROBE=1300`, then collect dirt2.log, mrt-probe/, and framedump/.
Find the first draw that changes valid RT1 values to zero; compare its RT0/Z
snapshots. If holes already exist in the first geometry draw, distinguish
varying/output corruption from raster rejection using the paired RT0/Z data.
Do not describe this diagnostic build as a fix until Switch evidence verifies it.

### Hardware trace retrieved; shader output discriminator installed

The user explicitly authorized FTP upload/download in chat (supersedes the old
handover's FTP restriction). Do not record credentials in source documents.

Original diagnostic run is saved intact in
`build/package/switch/evidence-mrt-1300/`: dirt2.log, all 633 complete probe raw
files, and all 30 end-of-frame dump files. Hardware Z after clear is exactly
0xFFFFFFFF in every pixel. Before the masked fullscreen draw, hardware Z is
written at every pixel (range 0xE3AE46FF..0xF05129FF), with smooth values even
where R32F is zero. R32F has 1,520,511 zeros out of 2,073,600 pixels.

Opaque draw 162 (VS e55d2b1ffe81, PS e6b631a7163b, 12 triangles) updates all
4096 hardware Z and RT0 pixels in the sampled 64x64 region. Its R32F result has
2848 zeros, including 749 pixels that had nonzero depth before this draw.
Therefore this draw actively produces zeros, rather than failing raster/depth
coverage. The depth clear, Zcull, fullscreen pass, and asynchronous hazards are
ruled out as explanations of this captured failure. The precise cause remains
between varying/compiler/output generation and MRT/R32F storage.

A second, shader-only diagnostic is compiled and uploaded (NRO unchanged).
Local artifact: `build/package/switch/mrt-output-test/dirt2.dkshpack`.
The normal production pack at `build/shaders/dx9/dirt2.dkshpack` is untouched.
Exactly the two depth-MRT fragment shaders e6b631a7163b and e80dc3fc05c7 are
replaced; their original input/output metadata and game lookup keys remain.
They now output the interpolated view depth to all RGB channels of RT0; RT1
outputs constant 0.25 on the left half (x<960), and interpolated view depth on
the right half, replicated to all channels. Alpha discard is preserved. UAM
TGSI for the opaque shader reduces to input reads and 13 instructions, so the
original motion-vector arithmetic is no longer in the executable shader.

This intentionally changes the screen and is NOT a graphical fix. Interpretation:
- Holes even in RT1's constant left half implicate output/storage or MRT state.
- Constant left half correct, depth right half wrong in both RT0 and RT1:
  investigate the varying/vertex stage.
- Both halves correct: investigate original fragment arithmetic/compiler and
  channel packing (this test also replicates depth to all output components).
- RT0 depth smooth but RT1 depth broken: investigate output1 generation/storage.

Uploaded env.txt has only DIRT2_FRAME_DUMP=1300, disabling the 633-file probe.
The preceding pack/settings have .before-output-test timestamped backups on
Switch. Awaiting the user to run and reopen FTP for the next frame dump.

### Output discriminator retrieved; minimal replicated-depth test installed

Second hardware capture is saved in `build/package/switch/evidence-output-test-1300/`
(log and all 30 framedump files). The broad R32F zero pattern is gone: left half
has 1,036,768 pixels exactly 0.25 out of 1,036,800; right half has no zeros.
There are 64 anomalous pixels total, all on the last row (including 32 left-half
pixels). Treat this small edge/readback anomaly separately from the original
1.5-million-pixel failure. RT0 depth is smooth in the previously broken crop.
This implicates original fragment arithmetic/output generation; it does not yet
identify which change fixed the broad failure.

Next test artifact: `build/package/switch/mrt-replicated-depth-test/`.
Exactly the same two fragment shaders are replaced, this time using ORIGINAL
motion-vector arithmetic, original RT0 outputs, and original alpha discard.
The sole GLSL change is replacing the last RT1 assignment with
`ps_oC1 = vec4(ps_v0.z);` (opaque) or `vec4(ps_v3.z);` (alpha).
This preserves the red component used by R32F, replicating it into unused GBA.
Production shader pack remains untouched. Pack SHA256:
`e53e9d090ee307237d1f4e20c53d63c0c517a7336b5034becb1fade033410f43`.
Uploaded via authorized FTP with verified bytes and timestamped backups;
env enables only DIRT2_FRAME_DUMP=1300. NRO unchanged. This is a candidate
workaround/discriminator, NOT yet a confirmed fix. Await another hardware run.
If successful, investigate output register allocation/zero-component export
and implement a justified shader-pipeline correction; avoid a global replication
transform that would change semantics for multichannel render targets.

Packaging verification: a second full compilation differed by one byte in an unrelated
shader (SHA-256 ed23639e568f…, DKSH offset 610, 0x20 vs 0x40). The first fresh
build at build/shaders/dx9-fixed-verification was exactly identical to the tested
pack. Standard output and package use that verified build, including its DKSH
for SHA-1 32d309f296ffee208b6c52492d20a41a8f12fd4b. Both corrected depth
shaders were identical in both rebuilds. Investigate UAM nondeterminism separately.

### New issue: torn alert panels (2026-10-19)

User supplied `build/package/switch/2026101915475100-899BF99A6A4D5CD93E86FC3EDF92F308.jpg`.
CREATE AUTOSAVE dialog has regular comb-like rectangular gaps on the upper panel
and green selection strip; text remains intact. No cause established or fix yet.
The previous clean-depth capture was on the PRESS START screen, not this alert.

Added opt-in `DIRT2_DEKO_UI_PROBE=<frame>` to the Switch Device. On that frame,
alpha-blended draws log VS/PS identities, target size, blend/alpha-test state,
texture dimensions/formats/usage and sampler addressing/filtering. Unique sampled
textures are captured at mip 0, layer 0 as tightly packed raw blocks in ui-probe/.
Readback occurs before state emission so bindings are restored after GPU flush.
The probe does not change shader packs; keep the verified depth correction.
Diagnostic build enables UI_PROBE=1800 and FRAME_DUMP=1800, giving user time to
open CREATE AUTOSAVE before capture. Local env is in build/package/switch/ui-diagnostic/.
User authorized FTP and has opened it; upload NRO+env with backups and checksum
verification, then request run with alert left open for around two minutes.
Next collect dirt2.log, ui-probe/, framedump/ into a NEW evidence folder.
Inspect panel texture(s) and shader(s) before choosing a fix. No subagents used.

### Alert UI capture retrieved; alpha-source diagnostic installed

Evidence: build/package/switch/evidence-ui-1800/ has log, 206 sampled raw textures,
and 30 framedump files. Final capture reproduces the comb edges. The relevant
panel texture is tex194_512x256_fmt861165636.raw (DXT3/BC2); it has a smooth,
slightly ragged alpha edge, not the comb pattern. Black/white texture alpha preview
is /tmp/dirt-panel-alpha.png; decoded texture PNGs 178..205 are alongside raw
files (Python decoder in /tmp/dirt_decode_ui.py). Do not assume texture contents
are corrupted merely from the rendered edge.

Last UI draws use PS c21164b921ae (SHA-1 d8aa5008674a01a4c07dc5d1434dc4e06546fd45),
simple texture(s0, vec2(input0.w,input1.w))*input2. Opaque panel uses
VS adfe4dcced05 or 8dc4bdf5ecce; font uses a separate PS 1532d1ed537b and A8 atlas.
Panel tex194 is sampled in draws 357 and 372. UI state is alpha test GREATER0,
blend SRCALPHA/INVSRCALPHA, clamp UV, linear filters. Many other sampled textures
are scene objects; first 193 are not this alert.

Next shader-only diagnostic build/package/switch/ui-alpha-test/ replaces only
PS c21164b921ae, retains normal production pack locally, and colours UI panels red.
Alpha is texture alpha for fragcoord.x<850, vertex alpha for 850<=x<1100,
and constant1 for x>=1100. Use final captured screenshot to identify which region
has holes: texture-only implicates sampling; vertex-only varying/VS; constant region
also broken implicates geometry/rasterization. This is NOT a fix.
Uploaded pack+env via authorized FTP, byteverified and backups retained. NRO
unchanged. Env now FRAME_DUMP=1800 only (no more 206-texture captures).
Ask user to run to CREATE AUTOSAVE and hold for ~two minutes, then reopen FTP.

### Alpha-source capture: texture-alpha path is responsible

Retrieved complete capture build/package/switch/evidence-ui-alpha-1800/.
Final image confirms comb gaps ONLY in x<850 (texture alpha). Vertex alpha
850..1100 and constant alpha >=1100 regions are solid. Geometry and vertex
alpha therefore do not explain the fault. Texture tex194 has normal decoded
alpha, so investigate compressed sampling / coordinates / texture instruction.

Next A/B adds DIRT2_DEKO_EXPAND_BC2=1: BC2 (D3D DXT2/3) static textures use
RGBA8 GPU images, retaining compressed D3D locks and decoding during upload.
Original normal shader pack restored; no shader bypass of alpha. Decoder handles
all 16 explicit 4-bit alphas, opaque BC1 colour interpolation regardless of endpoint
ordering, partial edge blocks, mips/cube layers/volume slices/subrect uploads.
This is opt-in, not a production fix until hardware verification. UI probe raw
format now follows RGBA8 storage if this option is enabled (prevents undersized
compressed-format readbacks). Normal static LockRect readback remains unsupported
as before. RT/depth textures are excluded from expansion.
Next run with EXPAND_BC2=1 and FRAME_DUMP=1800 only; collect alert frame again.
If fixed, compressed sampling/storage path implicated; if unchanged, inspect UV
interpolation/texture instruction instead and remove the expansion experiment.

BC2 A/B validation: Switch build passes; git diff --check passes. Compiled the
actual C++ upload decoder on host with allocation stubs (extracted unchanged
from device.cpp) and compared against independent Python decode of tex194:
all 131072 alpha values match exactly; RGB rounding differs at most1/255.
Known alpha ramp, reversed RGB endpoint ordering, 4x4/3x2/1x1 partial blocks pass.
Host harness /tmp/dirt-bc2-check.cpp and /tmp/dirt-bc2-check are temporary.
Build NRO, normal verified shader pack, and env stored in ui-bc2-test/ uploaded
with .before-bc2-test timestamped backups. This A/B restores original UI colours.

### Audit of leftover debugging (2026-10-19)

User asked to remove forgotten debug changes, specifically disabled Zcull.
- Restored DkQueueFlags_EnableZcull (deko3d default); the earlier disable did
  not affect the failure and is no longer justified. Needs Switch smoke test.
- Removed unused DIRT2_DEKO_MRT_NO_DEPTH and all depth-test override plumbing.
  It was never enabled in our tests; normal D3D depth state now always applies.
- Resource-kind and adapter-call tracing are now opt-in via
  DIRT2_DEKO_LOG_RESOURCES. Periodic FPS logs (Device and Switch bridge) are
  opt-in via DIRT2_DEKO_LOG_PERF. Error/fault logs remain enabled.
- Standard package now includes comment-only env.txt, preventing stale capture
  settings from surviving an overlay install. Standalone build/package/switch/
  env.txt also cleaned; original saved at debug-audit-backup/env.before-cleanup.txt.
  Historical diagnostic packages/evidence are intentionally retained unchanged.
- Frame dumps, UI/MRT probes, per-operation synchronization, screenshots and
  clear-only self-test are all opt-in and absent from normal env.txt.
- Retained render-to-texture barriers, shader-aware colour masks, target clears,
  deterministic vertex-output initialization and verified depth/BC2 workarounds.
  Corrected zero_outputs comment: prior claim that depth VS never wrote z was
  disproven. Generated shader bytes unchanged by the comment edit.
- Retained disabled offscreen render-target hardware compression: deko3d only
  decompresses presented swapchain images in this implementation. Enabling it
  without a decompression transition is a separate implementation task.
- Other agent's envmap/CMake/hooks edits were inspected only and preserved.

Normal package rebuilt. Console still has the previous A/B NRO and env with
EXPAND_BC2=1, FRAME_DUMP=1800 until user reopens FTP. IMPORTANT: install the new
NRO before comment-only env; the old A/B NRO requires explicit EXPAND_BC2=1.
Then verify clean menu/alert with Zcull enabled. Do not claim hardware validation
of the cleanup until that test. The two fixes themselves were user-verified.

### Name-entry crash: actual cause and keyboard fix (2026-10-19)

User requested Atmosphere crash_report retrieval after player-name entry crashed.
Evidence saved under build/package/switch/keyboard-crash/ (recent reports + jpgs
and dirt2.log). Latest report: 01792419903_03db12780bd84000.log, original build
ID 13182ec6ddd0c8f262f88a97f45d012382a3cbf7 (matched installed ELF before rebuild).
Log ends XShowKeyboardUI at314.727s, default Player, cchResult15.

IMPORTANT: the visible HID error is SECONDARY, not the primary cause. Main thread
aborts with2345-0008 (LibnxError_NotInitialized) in hidGetTouchScreenStates,
called by SDL_SWITCH_PollTouch. Worker34 is still waiting in swkbdShow. But
thread02 is already running process teardown: XShowKeyboardUI line187 ->
std::thread::detach -> __cxa_throw -> __terminate -> abort/_exit -> __libnx_exit
-> __appExit -> hidExit/appletExit. That explains why SDL sees missing HID.
Full executable branch scan found only __appExit calling hidExit.

Disassembly proves pthread_detach is a stub returning0x58; installed errno.h
maps88 toENOSYS. std::thread::detach throws on this error, then its still-joinable
thread destructor calls terminate. The previous native HOS keyboard implementation
was present, but async XLive code used std::thread(...).detach() on Switch.
Earlier working hypothesis of a standalone HID/app polling race is superseded.

Fix: shims/xlive/xlive.cpp queues a heap-owned KeyboardRequest directly with
SDL_RunOnMainThread(wait=false) on Switch; returns IO_PENDING and completes the
same guest overlap/event on confirmation/cancellation. Dispatch failure completes
with ERROR_FUNCTION_FAILED. unique_ptr owns callback context. Other platforms
retain their existing async implementation. No detached keyboard worker on Switch.

platform/switch/switch_platform.cpp uses SDL_RunOnMainThread(wait=true) to keep
all HOS keyboard operations on the SDL main thread (direct when already there).
It pauses game rendering for the modal applet and resumes for all normal/error
return paths. It still uses swkbdCreate/Configure/Show/Close; this is HOS input.

Validation: Switch build passed; diff check passed. Actual queued-request code
was extracted unchanged into a host harness /tmp/dirt-keyboard-request-test.cpp;
ASAN run passed pending-before-callback, success, cancel and dispatch-failure
completion assertions. Hardware name-entry test still required. New build ID
6b53e3ca81fb12eb4e9c3d597da9da1ef05f25f5. Standard package rebuilt. Upload new NRO
with verified bytes and .before-keyboard-fix backup; keep normal env and shaders.

### Manual GPU captures and race-loading mapping exhaustion (2026-10-19)

User requested Minus+Plus manual GPU dumps and then reported a race-loading crash.
New crash evidence: build/package/switch/race-loading-crash/01792421766_03db12780bd84000.log
and dirt2.log. Keyboard confirmation succeeded in this run. The PRIMARY error is
rt::fatal: mapping guest0x2FD50000+0x6C0000 failed0xCE01 during race load after
~1067s. Kernel description103 is ResourceExhausted. Visible HID NotInitialized
abort is again SECONDARY during fatal process teardown. Do not blame keyboard
or rendering just because main thread's final crash stack is in SDL touch.

Runtime mapper fix: reserve a second4GB code-alias address window beside guest
reservation; aliases use fixed offsets matching guest offsets instead of random
placement per allocation. Round physical backing outward to2MB blocks, capped
at16MB per host allocation, retaining the low64KB guard. This reduces small
mapping/page-table resource pressure (old mapper retained all64KB mappings).
Windows logical allocation bookkeeping remains unchanged; physical backing is
retained/zeroed on release as before. Includes rollback and exact failing SVC,
backed-byte and mapping-count diagnostics. Exact kernel allocation stage was not
logged by the old implementation; hardware race-load retry is required.

Manual capture API shims/d3d9/capture.h + bridge.cpp:
- Switch XInput Minus(BACK)+Plus(START) chord; edge-latched until BOTH released.
  Chord bits consumed while latched. Requests during pending/capture ignored.
- Short120ms rumble acknowledges; longer450ms rumble means complete; low900ms
  means partial/failure. Feedback takes priority over game rumble briefly.
- Request arms at next Present and captures NEXT COMPLETE frame, not mid-frame.
- Unique UTC/frame/suffix directory sdmc:/switch/dirt2/gpu-dumps/; exclusive
  creation prevents overwrites across runs. No env flag or timed capture needed.
- screen.bmp, per-target BMP+raw (all supported formats including HDR and alpha),
  capture.txt metadata/statistics/all shader pairs and first sampled target links,
  and a dirt2.log snapshot saved per capture. Partial readbacks/I/O reported.
- deko records first sampled texture mip0/layer0/slice0 contents <=16MiB (depth
  formats omitted), per-draw float constants (256VS float4 then224PS float4),
  shader IDs, blend/alpha/depth/sRGB states and sampler flags. Expanded BC2 dumps
  are labelled as RGBA8 storage. Capturing synchronizes and can alter timing.
- Historical timed flags still supported for scripted comparisons, default off.

Validation: Switch build + diff check passed. ASAN host tests using extracted
actual request/chord/folder code passed edge latch, full-release rearm, busy
coalescing, no-overwrite directories and folder-error recovery. Actual mapper
code passed aligned mapping/reuse, capped large runs, guard page,4GB upper edge,
and rollback for each of3 kernel failure stages with SVC mocks under ASAN.
Tests are temporary /tmp/dirt-manual-capture-test.cpp and dirt-map-memory-test.cpp.
Hardware capture and race-load retry still required. New build ID:
d286ada282dccd8b2e47e9f22f491ce6e25749b8. Matching ELF and nm symbols preserved at
build/package/switch/symbols/<buildid>.* for subsequent crash symbolication.
Normal package rebuilt. Upload current NRO with .before-manual-dumps backup and
byte verification; retain normal env and fixed shaders. User has opened FTP.

Reload-fix artifact: buildf6771aa870245b34e5605af70b17736550143c6c, NRO7936197bytes;
matching ELF/nm in build/package/switch-wine/symbols/, regenerated standalone zip
and manifest. Initial FTP STOR temporary NRO failed ConnectionReset before any
rename or result archive, then reconnect refused. Installed runner and results
are still unchanged. User asked asynchronously to reopen FTP. Must finish upload
of NRO, README, source zip with retrieve/hash validation + backup rename, then
archive remote results/ and create fresh results/ so next run exercises0->1.

FTP retries diagnostic: Mac en0 IPv4 192.168.1.94, but destination192.168.1.148
is routed via VPN utun6 (gateway10.255.0.242). Use local Wi-Fi socket binding:
socket.setsockopt(socket.IPPROTO_IP,25,socket.if_nametoindex('en0')) (Darwin
IP_BOUND_IF) for BOTH FTP control and passive data connections, without changing
system routes. This changes timeout to immediate ECONNREFUSED at .148:5000;
confirmed3 times. FTP service must be reopened / actual address confirmed.
No deployment or remote result archive has completed; old runner remains.

Fault-collection deployment attempt did NOT connect (timeout, no remote changes).
Route now en0 with HOST/REJECT/LLINFO (no ARP), not VPN; console unavailable.
Asked user asynchronously to reopen FTP/keepawake/sendIPifchanged. Lastinstalled
runner remains0570abcb (constantboundsfix). Both texturefix-only5fd067... and
new faultcaptureb1e0... have NOT been installed. Must install currentb1e0 package,
verifyfiles, append verifiedcrash186 and advancepending to187 as above. Do not
say deployment complete unless deployment-fault-capture.json exists with success.

### Switch performance: incremental constants (2026-10-20)

Implemented a focused programmable-shader upload optimization in the deko backend.
Identical Set*ShaderConstant calls do not dirty state (state-block recording still
runs first). Per-device/per-stage ConstantUpload mirrors compare packed registers
and send only the changed 16-byte-aligned span through PushConstants. Persistent
UBO contents survive shader/list/frame switches; allocation init resets mirrors.
Boolean padding is deterministic. REBIND_STATE forces full uploads for diagnostics.
LOG_PERF now includes sent/full constant bytes; this is not a measured FPS gain.

Validation: host ASAN/UBSAN deterministic cases plus 10,000 randomized replays
passed; Switch dirt2.nro cross-build passed (existing unrelated indentation
warnings). ccache was disabled for the build because its default cache lies
outside the writable sandbox. Package: build/package/switch-performance/DiRT2-switch.zip;
matching ELF/symbols retained in its symbols/ directory. No console deployment
or gameplay/FPS validation performed. Existing unrelated work was preserved.
Hardware next: compare the same menu/race and clock settings, enable
DIRT2_DEKO_LOG_PERF=1 in console env.txt for FPS and upload-byte evidence.
