# source-sha256: 1a8ac262195595f0371490363b1f2dc0695810f44d00b405b0ce6e956eb36764
यह अंग्रेज़ी README का मशीनी अनुवाद है, इसमें गलतियाँ हो सकती हैं, और अंग्रेज़ी README ही आधिकारिक है।

PokoMac @VERSION@ Linux, SteamOS और Steam Deck के लिए

1. Steam में Proton Experimental इंस्टॉल करें (Library, Tools; Steam Deck पर पहले Desktop Mode में जाएँ)।
2. PokoMac.AppImage चलाएँ और अपना डंप फ़ोल्डर चुनें (base/ और updates/<version>/, दोनों में exefs/ और romfs/) या कोई .nup. वर्ज़न 2.0.1a ज़रूरी है।
3. Set up दबाएँ। पहली बार यह अपने बिल्ड टूल डाउनलोड करता है (Python और Zig, लगभग 100 MB), इस PC पर pkp.exe और शेडर कैश बनाता है, Proton के ज़रिए। आप रोककर बाद में जारी रख सकते हैं।
4. Play दबाएँ। गेम Steam के Proton के ज़रिए चलता है।

अगर PokoMac.AppImage शुरू नहीं होता (FUSE के बिना सिस्टम): ./PokoMac.AppImage --appimage-extract-and-run
Steam से (Deck पर Game Mode): PokoMac.AppImage को non-Steam गेम के रूप में जोड़ें और लॉन्च विकल्प --play सेट करें।

अपडेट: नई रिलीज़ को अपने इंस्टॉल पर एक्सट्रैक्ट करें। अगर अपडेट गेम का कोड बदलता है, तो लॉन्चर बताता है: फिर Set up दबाएँ और यह pkp.exe दोबारा बनाता है। आपके शेडर और सेव बचे रहते हैं।

लाइसेंस: LICENSE.txt और licenses/.
