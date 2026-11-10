# ربيع الأندلس — د. محمود ماهر (TXT)

د «ربيع الأندلس» ټول کتاب (۳۵۱ د PDF مخونه، عصير الكتب، دوهم چاپ) متن ته اړول شوی.
Full text of the novel *Rabi' al-Andalus* by Dr. Mahmoud Maher (351 PDF pages), converted from a scanned PDF.

## فایلونه / Files

| فایل | څه دي |
|---|---|
| `rabi-al-andalus.txt` | ټول کتاب د یوه پرله‌پسې متن په توګه؛ پاراګرافونه سره نښتي، د مخونو شمېرې لرې شوې. Clean continuous text (~61,700 words). |
| `rabi-al-andalus_pages.txt` | مخ په مخ، کرښه په کرښه لکه په چاپي کتاب کې؛ هر مخ د `[PDF n — ص m]` نښه لري. Page-by-page, line-by-line as printed. |
| `pages/p-NNN.txt` | هر مخ جلا فایل (۳۵۱ فایلونه). One file per PDF page. |

د چاپ شوي مخ شمېره = د PDF مخ − ۱ (د ۱۱ مخ نه تر ۳۵۰).
Printed page number = PDF page − 1 (PDF pages 11–350). PDF pages 5, 7, 9, 85, 127, 225 are blank.

## څنګه جوړ شو / How it was made

1. **OCR engines.** The PDF has no text layer (300 dpi JBIG2 scans), so OCR was required.
   - **kraken 7.1.1**, the recognition engine inside eScriptorium, with the official
     OpenITI printed-Arabic model `arabic_best.mlmodel` (Zenodo 10.5281/zenodo.7050296):
     `kraken -I 'p-*.png' -o .txt segment -bl -d horizontal-rl ocr --reorder --base-dir R -m arabic_best.mlmodel`
   - **Tesseract 5.3.4** with `tessdata_best` Arabic.

   The eScriptorium web application itself (a Django/PostgreSQL/Celery server) was not
   deployed. Only its OCR engine and models were used, which give the same recognition results.
2. **Why the raw OCR was not used as-is.** On this typeface both engines made errors on
   roughly 1 word in 10. Typical errors were confusing dotted letters (ب/ي، ن/ت، غ/ف), losing
   hamza and shadda, misreading the في ligature, and reversed punctuation. That is far from
   "no mistakes".
3. **Page-by-page verification.** Every one of the 351 pages was transcribed against the page
   image. Unclear words and diacritics were checked again on enlarged crops.
4. **Three-way vote.** The verified text was aligned word by word with both OCR outputs.
   A word was flagged wherever kraken and Tesseract *agreed with each other* but disagreed
   with the transcription. 227 words were flagged. 223 were OCR misreads; the other 4 were
   checked on the image, and the 3 real transcription errors among them were fixed.
   A second pass looked for words that both engines saw but the transcription lacked, and
   found no omitted lines or words.
5. **Paragraphs across pages.** The page images were measured to see whether each page's
   first line is indented. A paragraph that continues onto the next page is joined only when
   the previous page's last line is full width and the next page's first line is not indented.

## یادونې / Notes

- املا، اعراب (تشكيل)، او ټکي لکه په چاپ کې ساتل شوي دي. د چاپ خپلې تېروتنې نه دي سمې شوې
  (مثلاً «الأمي محمد»، «عل ما يرام»، «رواي الأندلس»).
  Spelling, tashkeel and punctuation are kept exactly as printed, including the book's own typos.
- The "Numidia Library / Telegram" watermark stamped on the scanned covers is not part of the book
  and was left out.
- Tashkeel is the hardest part to verify automatically, because the OCR engines are unreliable
  on it. If a spot still looks wrong, compare it with the matching `pages/p-NNN.txt` and the PDF page.
