# source-sha256: 1a8ac262195595f0371490363b1f2dc0695810f44d00b405b0ce6e956eb36764
هذه ترجمة آلية لملف README الإنجليزي، وقد تحتوي على أخطاء. النسخة الإنجليزية هي المرجع.

PokoMac @VERSION@ لنظام Linux وSteamOS وSteam Deck

1. في Steam، ثبّت Proton Experimental (المكتبة، الأدوات؛ على Steam Deck بدّل إلى وضع سطح المكتب أولاً).
2. شغّل PokoMac.AppImage واختر مجلد نسختك المستخرجة (base/ و updates/<version>/، ولكل منهما exefs/ و romfs/) أو ملف .nup. الإصدار 2.0.1a مطلوب.
3. اضغط Set up. في المرة الأولى ينزّل أدوات البناء (Python وZig، نحو 100 ميغابايت)، ويبني pkp.exe وذاكرة الشيدرات على هذا الحاسوب، عبر Proton. يمكنك الإيقاف المؤقت والمتابعة لاحقاً.
4. اضغط Play. تعمل اللعبة عبر Proton من Steam.

إذا لم يبدأ PokoMac.AppImage (نظام بدون FUSE): ./PokoMac.AppImage --appimage-extract-and-run
من Steam (وضع الألعاب على Deck): أضف PokoMac.AppImage كلعبة غير تابعة لـ Steam مع خيارات التشغيل --play.

التحديث: استخرج الإصدار الجديد فوق تثبيتك. إذا غيّر التحديث شيفرة اللعبة، يخبرك المشغّل: اضغط Set up مرة أخرى وسيعيد بناء pkp.exe. تبقى شيدراتك وملفات حفظك.

التراخيص: LICENSE.txt و licenses/.
