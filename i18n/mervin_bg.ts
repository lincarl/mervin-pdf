<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="bg" sourcelanguage="en">
<context>
    <name>MainWindow</name>
    <message>
        <source>Enter the document password:</source>
        <translation>Въведете паролата на документа:</translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) in %1 already exist. Split wrote nothing.</source>
        <extracomment>Tray message after Split found that some of the files it would write, such as plan-002.pdf, already exist while its window was hidden in the tray, so it could not ask whether to replace them. %n is the number of those files, %1 the folder Split writes to. Split wrote no file.</extracomment>
        <translation>
            <numerusform>%n файл в папката %1 вече съществува. Нищо не е записано.</numerusform>
            <numerusform>%n файла в папката %1 вече съществуват. Нищо не е записано.</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) in %1 already exist. Replace them?</source>
        <extracomment>Asked after Split found that some of the files it would write, such as plan-002.pdf, already exist. %n is the number of those files, %1 the folder Split writes to. Nothing has been replaced yet. Yes replaces those files and writes the others; No writes no file.</extracomment>
        <translation>
            <numerusform>%n файл в папката %1 вече съществува. Да бъде ли заменен?</numerusform>
            <numerusform>%n файла в папката %1 вече съществуват. Да бъдат ли заменени?</numerusform>
        </translation>
    </message>
    <message>
        <source>&quot;%1&quot; changed after Split checked it, so Split stopped without replacing it.</source>
        <extracomment>Split checked each of its files before writing. %1 is the name of one that another program changed, created or removed before Split replaced it. Split left that file as it is and wrote none of the files after it. The files with lower numbers that Split wrote before stay.</extracomment>
        <translation>„%1“ е променен след проверката, затова разделянето спря, без да го замени.</translation>
    </message>
    <message>
        <source>Operation failed</source>
        <translation>Операцията е неуспешна</translation>
    </message>
    <message>
        <source>A password is required.</source>
        <translation>Изисква се парола.</translation>
    </message>
    <message>
        <source>The operation failed.

%1</source>
        <translation>Операцията се провали.

%1</translation>
    </message>
    <message>
        <source>Pages (e.g. &quot;%1&quot;, &quot;1-%2&quot;, &quot;1,3,5-9&quot;):</source>
        <extracomment>%1 is the word for every page (PageRange &quot;All&quot;), %2 the page count.</extracomment>
        <translation>Страници (напр. &quot;%1&quot;, &quot;1-%2&quot;, &quot;1,3,5-9&quot;):</translation>
    </message>
    <message>
        <source>Done</source>
        <extracomment>Title of the message after a new file was written.</extracomment>
        <translation>Готово</translation>
    </message>
    <message>
        <source>Saved to:
%1

Open it now?</source>
        <translation>Запазено в:
%1

Отворете го сега?</translation>
    </message>
    <message>
        <source>&amp;Rotate Pages</source>
        <translation>&amp;Завъртане на страници</translation>
    </message>
    <message>
        <source>&amp;Delete Pages</source>
        <translation>&amp;Изтриване на страници</translation>
    </message>
    <message>
        <source>&amp;Extract Pages</source>
        <translation>&amp;Извличане на страници</translation>
    </message>
    <message>
        <source>Split All Pages into One File Each</source>
        <translation>Запазете всяка страница като отделен файл</translation>
    </message>
    <message>
        <source>&amp;Merge PDFs</source>
        <translation>&amp;Обединяване на PDF файлове</translation>
    </message>
    <message>
        <source>&amp;Security</source>
        <extracomment>Menu item that opens the document&apos;s password and permission settings.</extracomment>
        <translation>&amp;сигурност</translation>
    </message>
    <message>
        <source>Save as Copy</source>
        <translation>Запазване като копие</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf)</source>
        <extracomment>File type filter. Keep the pattern in parentheses.</extracomment>
        <translation>PDF документи (*.pdf)</translation>
    </message>
    <message>
        <source>Could not finish the print job.</source>
        <extracomment>The printer system rejected completion of the print job. It may have accepted earlier pages.</extracomment>
        <translation>Заданието за печат не може да бъде завършено.</translation>
    </message>
    <message>
        <source>Choose a different file name - use Save edits to write back to the original.</source>
        <extracomment>&quot;Save edits&quot; is a command in the Save menu. Use its translation here.</extracomment>
        <translation>Изберете друго име на файл - използвайте Запазване на редакциите, за да запишете обратно към оригинала.</translation>
    </message>
    <message>
        <source>The destination is open in a tab.</source>
        <translation>Дестинацията е отворена в раздел.</translation>
    </message>
    <message>
        <source>Could not save:
%1</source>
        <translation>Не може да се запази:
%1</translation>
    </message>
    <message>
        <source>Password required</source>
        <translation>Изисква се парола</translation>
    </message>
    <message>
        <source>Enter the password for %1.</source>
        <extracomment>%1 is the document&apos;s name. This asks for the password needed to open its contents, not the owner password that guards permission flags.
----------
%1 is the name of the document being opened or resumed. This asks for the password needed to open its contents, not its owner password.</extracomment>
        <translation>Въведете паролата за %1.</translation>
    </message>
    <message>
        <source>Save</source>
        <extracomment>Toolbar button (verb) that opens a menu of ways to save.</extracomment>
        <translation>Запазване</translation>
    </message>
    <message>
        <source>Close the other view of this file before saving, or save a copy.</source>
        <translation>Затворете другия изглед на този файл, преди да го запазите, или запазете копие.</translation>
    </message>
    <message>
        <source>File changed</source>
        <translation>Файлът е променен</translation>
    </message>
    <message>
        <source>%1 changed on disk since it was opened. Replace it with your edited copy?</source>
        <extracomment>%1 is the file name.</extracomment>
        <translation>%1 е променен на диска след отварянето му. Да го замените с вашето редактирано копие?</translation>
    </message>
    <message>
        <source>%1
Edited document retained at %2.
%3</source>
        <extracomment>%1 describes the Save or reopening result, %2 is the retained copy, and %3 is a recovery error.</extracomment>
        <translation>%1
Редактираният документ е запазен в %2.
%3</translation>
    </message>
    <message>
        <source>Don&apos;t save</source>
        <extracomment>Button in the unsaved-changes question. Closes the document and loses the changes made since it was last saved. The file on disk stays as it is.</extracomment>
        <translation>Без запазване</translation>
    </message>
    <message numerus="yes">
        <source>Wrote %n file(s) to:
%1</source>
        <extracomment>%1 is the folder the files were written to.</extracomment>
        <translation>
            <numerusform>Записан %n файл в:
%1</numerusform>
            <numerusform>Записани %n файла в:
%1</numerusform>
        </translation>
    </message>
    <message>
        <source>The file was saved, but could not be reopened. The edited copy remains open.</source>
        <translation>Файлът беше запазен, но не можа да бъде отворен отново. Редактираното копие остава отворено.</translation>
    </message>
    <message>
        <source>Saved %1</source>
        <extracomment>Status bar text. %1 is the saved file&apos;s path.</extracomment>
        <translation>Запазено %1</translation>
    </message>
    <message>
        <source>Unsaved changes</source>
        <translation>Незапазени промени</translation>
    </message>
    <message>
        <source>Save changes to %1?</source>
        <extracomment>%1 is the file name.</extracomment>
        <translation>Запазване на промените в %1?</translation>
    </message>
    <message>
        <source>Export with Measurements</source>
        <translation>Експортиране с измервания</translation>
    </message>
    <message>
        <source>There are no measurements to export. Add measurements first.</source>
        <translation>Няма мерки за експортиране. Първо добавете измервания.</translation>
    </message>
    <message>
        <source>Export</source>
        <extracomment>Title of messages about exporting with measurements.</extracomment>
        <translation>Експортиране</translation>
    </message>
    <message>
        <source>Could not export:
%1</source>
        <translation>Не можа да се експортира:
%1</translation>
    </message>
    <message>
        <source>The file was saved, but could not be reopened.</source>
        <extracomment>The original file was replaced successfully, but reading it again failed. The retained edited copy may still be loading for recovery.</extracomment>
        <translation>Файлът беше запазен, но не можа да бъде отворен отново.</translation>
    </message>
    <message>
        <source>Rotate Pages</source>
        <translation>Завъртане на страници</translation>
    </message>
    <message>
        <source>90° clockwise</source>
        <translation>90° по часовниковата стрелка</translation>
    </message>
    <message>
        <source>90° counter-clockwise</source>
        <translation>90° обратно на часовниковата стрелка</translation>
    </message>
    <message>
        <source>Rotation:</source>
        <translation>Въртене:</translation>
    </message>
    <message>
        <source>Save Rotated Copy</source>
        <translation>Запазване на завъртяно копие</translation>
    </message>
    <message>
        <source>Choose Output Folder</source>
        <translation>Изберете Изходна папка</translation>
    </message>
    <message>
        <source>Split Pages</source>
        <translation>Разделяне на страници</translation>
    </message>
    <message>
        <source>Print</source>
        <extracomment>Title of the print dialog and of messages about printing.
----------
Tooltip of the toolbar&apos;s print button (verb).</extracomment>
        <translation>Печат</translation>
    </message>
    <message>
        <source>Could not prepare the document:
%1</source>
        <translation>Документът не можа да се подготви:
%1</translation>
    </message>
    <message>
        <source>Could not start the print job.</source>
        <translation>Заданието за печат не можа да започне.</translation>
    </message>
    <message>
        <source>Could not start the next printed page.</source>
        <translation>Не може да започне следващата отпечатана страница.</translation>
    </message>
    <message>
        <source>Could not render page %1.</source>
        <translation>Страница %1 не можа да се изобрази.</translation>
    </message>
    <message>
        <source>Recent</source>
        <extracomment>Button left of the document tabs that shows the recently opened files.</extracomment>
        <translation>Скорошни</translation>
    </message>
    <message>
        <source>Duplicate to new window</source>
        <translation>Дублиране в нов прозорец</translation>
    </message>
    <message>
        <source>Move to new window</source>
        <translation>Преместване в нов прозорец</translation>
    </message>
    <message>
        <source>Close all tabs</source>
        <translation>Затворете всички раздели</translation>
    </message>
    <message>
        <source>Mervin PDF</source>
        <extracomment>Window title while no document is open. The product name.</extracomment>
        <translation>Mervin PDF</translation>
    </message>
    <message>
        <source>Open</source>
        <extracomment>Toolbar button (verb): open a PDF file in a new tab.</extracomment>
        <translation>Отвори</translation>
    </message>
    <message>
        <source>Open in New Window</source>
        <translation>Отворете в нов прозорец</translation>
    </message>
    <message>
        <source>Save edits</source>
        <translation>Запазване на редакциите</translation>
    </message>
    <message>
        <source>Save measurements, annotations and filled form fields into this file</source>
        <translation>Запазете измерванията, анотациите и попълнените полета на формуляра в този файл</translation>
    </message>
    <message>
        <source>Save as copy…</source>
        <translation>Запазване като копие...</translation>
    </message>
    <message>
        <source>Save the document with its edits to a new file</source>
        <translation>Запазете документа с неговите редакции в нов файл</translation>
    </message>
    <message>
        <source>Export with measurements…</source>
        <translation>Експортиране с измервания...</translation>
    </message>
    <message>
        <source>Export a copy with the measurements burned in as non-editable marks</source>
        <translation>Експортирайте копие с измерванията, записани като нередактируеми маркировки</translation>
    </message>
    <message>
        <source>New &amp;Window</source>
        <translation>&amp;Нов прозорец</translation>
    </message>
    <message>
        <source>&amp;New Tab</source>
        <translation>&amp;Нов раздел</translation>
    </message>
    <message>
        <source>&amp;Close Tab</source>
        <translation>&amp;Затваряне на раздела</translation>
    </message>
    <message>
        <source>Reopen Closed &amp;Tab</source>
        <translation>&amp;Повторно отваряне на затворен раздел</translation>
    </message>
    <message>
        <source>Next Tab</source>
        <translation>Следващ раздел</translation>
    </message>
    <message>
        <source>Previous Tab</source>
        <translation>Предишен раздел</translation>
    </message>
    <message>
        <source>Previous</source>
        <extracomment>Toolbar button: go to the previous page.</extracomment>
        <translation>Предишен</translation>
    </message>
    <message>
        <source>Next</source>
        <extracomment>Toolbar button: go to the next page.</extracomment>
        <translation>Следваща</translation>
    </message>
    <message>
        <source>&amp;Find</source>
        <extracomment>Verb: search the document&apos;s text.</extracomment>
        <translation>&amp;Намерете</translation>
    </message>
    <message>
        <source>Find &amp;Next</source>
        <translation>&amp;Намиране на следващия</translation>
    </message>
    <message>
        <source>Find &amp;Previous</source>
        <translation>&amp;Намерете предишния</translation>
    </message>
    <message>
        <source>Find</source>
        <extracomment>Toolbar button (verb): open the find field over the document.</extracomment>
        <translation>Намерете</translation>
    </message>
    <message>
        <source>Find (%1)</source>
        <extracomment>Tooltip of the toolbar&apos;s find button. %1 is its keyboard shortcut, such as Ctrl+F.</extracomment>
        <translation>Намиране (%1)</translation>
    </message>
    <message>
        <source>&amp;Copy</source>
        <extracomment>Verb: copy the selected text.</extracomment>
        <translation>&amp;Копирай</translation>
    </message>
    <message>
        <source>Select &amp;All</source>
        <translation>&amp;Изберете Всички</translation>
    </message>
    <message>
        <source>&amp;OCR Selection</source>
        <extracomment>OCR is optical character recognition: turning an image of text into text. This tool recognises the text in a region the user selects.</extracomment>
        <translation>&amp;Избор на OCR</translation>
    </message>
    <message>
        <source>OCR Selection: drag a region to recognise its text</source>
        <translation>Избор на OCR: плъзнете регион, за да разпознаете неговия текст</translation>
    </message>
    <message>
        <source>&amp;Measure</source>
        <extracomment>Turns the measuring tool on or off, for distances, paths, areas and angles on the page. Measure is the verb, so use the verb or the word for measuring, not a noun for a step, a unit or a size.</extracomment>
        <translation>&amp;Измерете</translation>
    </message>
    <message>
        <source>Measure</source>
        <extracomment>Tooltip of the toolbar&apos;s measuring tool button. Use the same word as the Measure action, the verb or the word for measuring.</extracomment>
        <translation>Измерете</translation>
    </message>
    <message>
        <source>Fill &amp;Forms</source>
        <translation>&amp;Попълнете формуляри</translation>
    </message>
    <message>
        <source>Comme&amp;nt</source>
        <extracomment>Toggles the comment tool, which highlights text and adds sticky-note comments.</extracomment>
        <translation>&amp;Коментар</translation>
    </message>
    <message>
        <source>Comment: highlight text and add sticky-note comments</source>
        <translation>Коментар: маркирайте текст и добавете коментари като лепкави бележки</translation>
    </message>
    <message>
        <source>Highlight Form Fields</source>
        <extracomment>Menu toggle: tint the fillable form fields of the document.</extracomment>
        <translation>Маркирайте полетата на формуляра</translation>
    </message>
    <message>
        <source>Zoom In</source>
        <translation>Увеличете</translation>
    </message>
    <message>
        <source>Zoom Out</source>
        <translation>Намаляване</translation>
    </message>
    <message>
        <source>Fit Page / Fit Width</source>
        <extracomment>Keyboard command that switches between fitting the whole page and the page width to the window.</extracomment>
        <translation>Побери страница / ширина</translation>
    </message>
    <message>
        <source>Rotate Left</source>
        <translation>Завъртете наляво</translation>
    </message>
    <message>
        <source>Rotate Right</source>
        <translation>Завъртане надясно</translation>
    </message>
    <message>
        <source>&amp;Continuous Scroll</source>
        <translation>&amp;Непрекъснато превъртане</translation>
    </message>
    <message>
        <source>&amp;Single Page Scroll</source>
        <translation>&amp;Превъртане на една страница</translation>
    </message>
    <message>
        <source>&amp;Two-Page Spread</source>
        <translation>&amp;Разширение от две страници</translation>
    </message>
    <message>
        <source>&amp;Full Screen</source>
        <translation>&amp;Цял екран</translation>
    </message>
    <message>
        <source>Always on &amp;Top</source>
        <translation>&amp;Винаги на върха</translation>
    </message>
    <message>
        <source>&amp;Settings</source>
        <translation>&amp;Настройки</translation>
    </message>
    <message>
        <source>&amp;Print</source>
        <extracomment>Verb: print the document.</extracomment>
        <translation>&amp;Печат</translation>
    </message>
    <message>
        <source>Main</source>
        <extracomment>Name of the main toolbar, listed when the user right-clicks the toolbar area.</extracomment>
        <translation>Основен</translation>
    </message>
    <message>
        <source>Menu</source>
        <extracomment>Tooltip of the toolbar button that opens the main menu.</extracomment>
        <translation>Меню</translation>
    </message>
    <message>
        <source>Save edits, save a copy, or export with measurements</source>
        <translation>Запазете редакциите, запазете копие или експортирайте с измервания</translation>
    </message>
    <message>
        <source>Document</source>
        <extracomment>Toolbar button (noun) that opens a menu of page operations and document security.</extracomment>
        <translation>Документ</translation>
    </message>
    <message>
        <source>Rotate, delete, extract, split or merge pages; document security</source>
        <translation>Завъртане, изтриване, извличане, разделяне или сливане на страници; сигурност на документите</translation>
    </message>
    <message>
        <source>Fit Page</source>
        <extracomment>Zoom choice: scale the page so the whole page fits the window.</extracomment>
        <translation>Побери страницата</translation>
    </message>
    <message>
        <source>Fit Width</source>
        <extracomment>Zoom choice: scale the page so its width fits the window.</extracomment>
        <translation>Побери ширината</translation>
    </message>
    <message>
        <source>View</source>
        <extracomment>Section heading in the main menu (noun): how the document is shown.</extracomment>
        <translation>Изглед</translation>
    </message>
    <message>
        <source>Fit &amp;Page</source>
        <extracomment>Zoom command that fits the whole page in the window. The ampersand marks the menu keyboard access key.</extracomment>
        <translation>&amp;Побери страницата</translation>
    </message>
    <message>
        <source>Fit &amp;Width</source>
        <extracomment>Zoom command that fits the page width in the window. The ampersand marks the menu keyboard access key.</extracomment>
        <translation>&amp;Побери ширината</translation>
    </message>
    <message>
        <source>Scroll</source>
        <extracomment>Section heading in the main menu (noun): how the pages scroll, continuously or one page at a time.</extracomment>
        <translation>Превъртане</translation>
    </message>
    <message>
        <source>Panels</source>
        <extracomment>Section heading in the main menu: the side panels (outline, thumbnails, comments).</extracomment>
        <translation>Панели</translation>
    </message>
    <message>
        <source>Select &amp; Annotate</source>
        <extracomment>Section heading in the main menu: selecting text and the comment, OCR, measuring and form tools. Plain menu section heading. The ampersand means &quot;and&quot;, not a keyboard mnemonic.</extracomment>
        <translation>Изберете текст и анотирайте</translation>
    </message>
    <message>
        <source>Switch to the traditional document theme</source>
        <extracomment>Traditional and Comfort are the document theme names in Settings &gt; Appearance.</extracomment>
        <translation>Преминете към традиционната тема на документа</translation>
    </message>
    <message>
        <source>Switch to the comfort (dark) document theme</source>
        <extracomment>Traditional and Comfort are the document theme names in Settings &gt; Appearance.</extracomment>
        <translation>Превключете към удобната (тъмна) тема на документа</translation>
    </message>
    <message>
        <source>Password Required</source>
        <translation>Изисква се парола</translation>
    </message>
    <message>
        <source>Open from URL</source>
        <translation>Отворете от URL</translation>
    </message>
    <message>
        <source>Could not create the download folder.</source>
        <translation>Не можа да се създаде папка за изтегляне.</translation>
    </message>
    <message>
        <source>Downloading %1...</source>
        <extracomment>%1 is the name of the file being downloaded.</extracomment>
        <translation>Изтегля се %1...</translation>
    </message>
    <message>
        <source>Cancel</source>
        <translation>Отказ</translation>
    </message>
    <message>
        <source>No recently closed tab to reopen</source>
        <translation>Няма наскоро затворен раздел за повторно отваряне</translation>
    </message>
    <message>
        <source>%1 - Mervin PDF</source>
        <extracomment>Window title. %1 is the name of the document shown.</extracomment>
        <translation>%1 - Mervin PDF</translation>
    </message>
    <message>
        <source>Save Page As</source>
        <extracomment>Context menu item: save the clicked page as a new PDF.</extracomment>
        <translation>Запазване на страницата като</translation>
    </message>
    <message>
        <source>Outline</source>
        <extracomment>Side panel title: the document&apos;s table of contents (bookmarks).</extracomment>
        <translation>Съдържание</translation>
    </message>
    <message>
        <source>Thumbnails</source>
        <extracomment>Side panel title: small page previews.</extracomment>
        <translation>Миниатюри</translation>
    </message>
    <message>
        <source>Comments</source>
        <extracomment>Side panel title (noun, plural): the list of the document&apos;s comments.</extracomment>
        <translation>Коментари</translation>
    </message>
    <message>
        <source>OCR Selection</source>
        <extracomment>Message box title. OCR is optical character recognition.</extracomment>
        <translation>Избор на OCR</translation>
    </message>
    <message>
        <source>No OCR language data is installed.

Open the language manager to add a language (e.g. English)?</source>
        <translation>Не са инсталирани OCR езикови данни.

Отваряне на езиковия мениджър, за да добавите език (напр. английски)?</translation>
    </message>
    <message>
        <source>Recognizing…</source>
        <translation>Разпознаване...</translation>
    </message>
    <message>
        <source>[OCR failed: %1]</source>
        <extracomment>Shown in place of the recognised text. %1 is the error message.</extracomment>
        <translation>[OCR неуспешно: %1]</translation>
    </message>
    <message>
        <source>%1 remains loaded. %2</source>
        <extracomment>Shown when an inactive document could not be unloaded from memory. %1 is the document name, %2 the reason.</extracomment>
        <translation>%1 остава зареден. %2</translation>
    </message>
</context>
<context>
    <name>MeasureContent</name>
    <message>
        <source>%1 (paper)</source>
        <extracomment>%1 is a measured value with its unit, such as &quot;12.50 mm&quot; (an area adds its perimeter on a second line). The page has no drawing scale, so the value is the size on the paper.</extracomment>
        <translation>%1 (хартия)</translation>
    </message>
</context>
<context>
    <name>PageRange</name>
    <message>
        <source>Enter pages like 1-3, 5, 8-10.</source>
        <translation>Въведете страници като 1-3, 5, 8-10.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; is not a valid page number.</source>
        <extracomment>%1 is what the user typed between two commas.</extracomment>
        <translation>„%1“ не е валиден номер на страница.</translation>
    </message>
    <message>
        <source>Page %1 is out of range (1-%2).</source>
        <extracomment>%1 is the page number typed, %2 the document&apos;s last page.</extracomment>
        <translation>Страница %1 е извън обхват (1-%2).</translation>
    </message>
    <message>
        <source>&quot;%1&quot; is not a valid page range.</source>
        <extracomment>%1 is what the user typed between two commas, like &quot;4-x&quot;.</extracomment>
        <translation>„%1“ не е валиден диапазон от страници.</translation>
    </message>
    <message>
        <source>Range &quot;%1&quot; is out of range (1-%2).</source>
        <extracomment>%1 is the range typed, like &quot;30-40&quot;, %2 the document&apos;s last page.</extracomment>
        <translation>Диапазонът &quot;%1&quot; е извън обхвата (1-%2).</translation>
    </message>
    <message>
        <source>Range &quot;%1&quot; is backwards - write it as low-high.</source>
        <extracomment>%1 is the range typed, like &quot;12-1&quot;.</extracomment>
        <translation>Диапазон &quot;%1&quot; е назад - запишете го като ниско-високо.</translation>
    </message>
    <message>
        <source>All</source>
        <extracomment>Typed in a page range field to mean every page, and a merge row&apos;s default range. One short word without commas or semicolons.</extracomment>
        <translation>Всички</translation>
    </message>
</context>
<context>
    <name>PrintDialog</name>
    <message>
        <source>Print</source>
        <extracomment>Dialog title.
----------
Button: starts printing.</extracomment>
        <translation>Печат</translation>
    </message>
    <message>
        <source>Printer</source>
        <translation>Принтер</translation>
    </message>
    <message>
        <source>%1 (%2)</source>
        <extracomment>A printer in the list: %1 is its name, %2 its description.</extracomment>
        <translation>%1 (%2)</translation>
    </message>
    <message>
        <source>No printers available</source>
        <translation>Няма налични принтери</translation>
    </message>
    <message>
        <source>Name:</source>
        <extracomment>Label of the printer list.</extracomment>
        <translation>Име:</translation>
    </message>
    <message>
        <source>Paper size:</source>
        <translation>Размер на хартията:</translation>
    </message>
    <message>
        <source>Print to a PDF file instead</source>
        <translation>Вместо това отпечатайте в PDF файл</translation>
    </message>
    <message>
        <source>Orientation</source>
        <translation>Ориентация</translation>
    </message>
    <message>
        <source>Portrait</source>
        <translation>Портретна</translation>
    </message>
    <message>
        <source>Landscape</source>
        <translation>Пейзажна</translation>
    </message>
    <message>
        <source>Pages</source>
        <extracomment>Group heading over the choice of which pages to print.</extracomment>
        <translation>Страници</translation>
    </message>
    <message>
        <source>All pages (page 1-%1)</source>
        <extracomment>%1 is the document&apos;s last page.</extracomment>
        <translation>Всички страници (страница 1-%1)</translation>
    </message>
    <message>
        <source>All pages (page 1)</source>
        <translation>Всички страници (страница 1)</translation>
    </message>
    <message>
        <source>Current page (page %1)</source>
        <extracomment>%1 is the page number shown in the viewer.</extracomment>
        <translation>Текуща страница (страница %1)</translation>
    </message>
    <message>
        <source>Pages from</source>
        <extracomment>Radio button followed by a page number field, then &quot;to&quot; and another page number field: &quot;Pages from [1] to [12]&quot;.</extracomment>
        <translation>Страници от</translation>
    </message>
    <message>
        <source>to</source>
        <extracomment>Between the two page number fields of &quot;Pages from [1] to [12]&quot;.</extracomment>
        <translation>до</translation>
    </message>
    <message>
        <source>e.g. 1-3, 5, 8-10</source>
        <translation>напр. 1-3, 5, 8-10</translation>
    </message>
    <message>
        <source>Options</source>
        <translation>Опции</translation>
    </message>
    <message>
        <source>Copies:</source>
        <translation>Копия:</translation>
    </message>
    <message>
        <source>Colour</source>
        <extracomment>Colour mode option: print in colour.</extracomment>
        <translation>Цвят</translation>
    </message>
    <message>
        <source>Grayscale</source>
        <translation>Скала на сивото</translation>
    </message>
    <message>
        <source>Black and white</source>
        <translation>Черно и бяло</translation>
    </message>
    <message>
        <source>Colour:</source>
        <translation>цвят:</translation>
    </message>
    <message>
        <source>Draft (150 dpi)</source>
        <extracomment>Print quality option. dpi is dots per inch.</extracomment>
        <translation>Чернова (150 dpi)</translation>
    </message>
    <message>
        <source>Normal (300 dpi)</source>
        <translation>Нормално (300 dpi)</translation>
    </message>
    <message>
        <source>High (600 dpi)</source>
        <translation>Висок (600 dpi)</translation>
    </message>
    <message>
        <source>Quality:</source>
        <translation>качество:</translation>
    </message>
    <message>
        <source>Fit to page</source>
        <translation>Побиране в страницата</translation>
    </message>
    <message>
        <source>Actual size</source>
        <translation>Реален размер</translation>
    </message>
    <message>
        <source>Custom</source>
        <comment>page selection</comment>
        <extracomment>Page selection option: print the pages typed in the field next to it.</extracomment>
        <translation>Персонализирани настройки</translation>
    </message>
    <message>
        <source>Custom</source>
        <comment>scale</comment>
        <extracomment>Scale option: print at the percentage set next to it.</extracomment>
        <translation>Персонализирани настройки</translation>
    </message>
    <message>
        <source>Scale:</source>
        <translation>Мащаб:</translation>
    </message>
    <message>
        <source>Centered</source>
        <translation>Центрирано</translation>
    </message>
    <message>
        <source>Centered vertically</source>
        <translation>Центрирано вертикално</translation>
    </message>
    <message>
        <source>Centered horizontally</source>
        <translation>Центрирано хоризонтално</translation>
    </message>
    <message>
        <source>Top left</source>
        <translation>Горе вляво</translation>
    </message>
    <message>
        <source>Alignment:</source>
        <translation>Подравняване:</translation>
    </message>
    <message>
        <source>Print on both sides</source>
        <translation>Печат от двете страни</translation>
    </message>
    <message>
        <source>Two-sided:</source>
        <translation>Двустранно:</translation>
    </message>
    <message>
        <source>Preview</source>
        <extracomment>Heading over the print preview.</extracomment>
        <translation>Преглед</translation>
    </message>
    <message>
        <source>Previous preview page</source>
        <translation>Предишна страница за преглед</translation>
    </message>
    <message>
        <source>Next preview page</source>
        <translation>Следваща страница за визуализация</translation>
    </message>
    <message>
        <source>Dashed line shows the printable area</source>
        <translation>Пунктираната линия показва областта за печат</translation>
    </message>
    <message>
        <source>Print using system dialogue…</source>
        <translation>Печат чрез системен диалог...</translation>
    </message>
    <message>
        <source>Cancel</source>
        <translation>Отказ</translation>
    </message>
    <message>
        <source>This document has no pages to print.</source>
        <translation>Този документ няма страници за печат.</translation>
    </message>
    <message>
        <source>Choose a printer or print to a PDF file.</source>
        <translation>Изберете принтер или отпечатайте в PDF файл.</translation>
    </message>
    <message>
        <source>This printer does not support the selected paper size.</source>
        <translation>Този принтер не поддържа избрания размер хартия.</translation>
    </message>
    <message>
        <source>The selected paper size cannot use these printer margins.</source>
        <translation>Избраният размер хартия не може да използва тези полета на принтера.</translation>
    </message>
    <message>
        <source>No pages selected</source>
        <translation>Няма избрани страници</translation>
    </message>
    <message>
        <source>Page %1 of %2</source>
        <extracomment>Under the preview: %1 is the page shown, %2 the document&apos;s page count.</extracomment>
        <translation>Страница %1 от %2</translation>
    </message>
    <message>
        <source>Page %1 · %2 of %3 selected</source>
        <extracomment>Under the preview: %1 is the page shown, which is number %2 of the %3 pages selected for printing.</extracomment>
        <translation>Избрана страница %1 · %2 от %3</translation>
    </message>
    <message>
        <source>document</source>
        <extracomment>Default file name, without &quot;.pdf&quot;, when the document has none.</extracomment>
        <translation>документ</translation>
    </message>
    <message>
        <source>Print to file</source>
        <translation>Печат във файл</translation>
    </message>
    <message>
        <source>PDF files (*.pdf)</source>
        <extracomment>File type filter. Keep &quot;(*.pdf)&quot;.</extracomment>
        <translation>PDF файлове (*.pdf)</translation>
    </message>
</context>
<context>
    <name>QShortcut</name>
    <message>
        <source>Home</source>
        <extracomment>Key name in the keyboard shortcut list. Write it as printed on the key or as the OS documents it. Left unfinished, the English name shows.</extracomment>
        <translation>Home</translation>
    </message>
    <message>
        <source>Up</source>
        <extracomment>Key name in the keyboard shortcut list: the up arrow key. Write it as the OS documents it. Left unfinished, the English name shows.</extracomment>
        <translation>нагоре</translation>
    </message>
    <message>
        <source>Down</source>
        <extracomment>Key name in the keyboard shortcut list: the down arrow key. Write it as the OS documents it. Left unfinished, the English name shows.</extracomment>
        <translation>Надолу</translation>
    </message>
</context>
<context>
    <name>SettingsDialog</name>
    <message>
        <source>Open</source>
        <extracomment>Keyboard shortcut list (verb): open a PDF file.</extracomment>
        <translation>Отвори</translation>
    </message>
    <message>
        <source>Save edits</source>
        <translation>Запазване на редакциите</translation>
    </message>
    <message>
        <source>New window</source>
        <extracomment>Keyboard shortcut list (command): open a new window.</extracomment>
        <translation>Нов прозорец</translation>
    </message>
    <message>
        <source>New tab</source>
        <translation>Нов раздел</translation>
    </message>
    <message>
        <source>Close tab</source>
        <translation>Затваряне на раздела</translation>
    </message>
    <message>
        <source>Reopen closed tab</source>
        <translation>Повторно отваряне на затворен раздел</translation>
    </message>
    <message>
        <source>Cycle tabs</source>
        <extracomment>Keyboard shortcut list: switch to the next or previous tab.</extracomment>
        <translation>Превключване на раздели</translation>
    </message>
    <message>
        <source>Find</source>
        <extracomment>Keyboard shortcut list (verb): search the document&apos;s text.</extracomment>
        <translation>Намерете</translation>
    </message>
    <message>
        <source>Find next / previous</source>
        <translation>Намиране на следващ / предишен</translation>
    </message>
    <message>
        <source>Previous / next page</source>
        <translation>Предишна / следваща страница</translation>
    </message>
    <message>
        <source>Zoom in / out</source>
        <translation>Увеличаване / намаляване</translation>
    </message>
    <message>
        <source>Fit page / width</source>
        <extracomment>Keyboard shortcut list: fit the whole page, or the page width, to the window.</extracomment>
        <translation>Побиране на страница / ширина</translation>
    </message>
    <message>
        <source>Toggle fit page / width</source>
        <extracomment>Keyboard shortcut list: switch between fitting the page and the page width.</extracomment>
        <translation>Превключване на побиране на страница/ширина</translation>
    </message>
    <message>
        <source>Rotate left / right</source>
        <translation>Завъртете наляво/надясно</translation>
    </message>
    <message>
        <source>Copy / Select all</source>
        <extracomment>Keyboard shortcut list (verbs): copy the selected text / select all text.</extracomment>
        <translation>Копиране / Избиране на всички</translation>
    </message>
    <message>
        <source>Comment</source>
        <extracomment>Keyboard shortcut list: turn the comment tool on or off.</extracomment>
        <translation>Коментар</translation>
    </message>
    <message>
        <source>OCR selection</source>
        <extracomment>Keyboard shortcut list: recognise the text in a selected region. OCR is optical character recognition.</extracomment>
        <translation>Избор на OCR</translation>
    </message>
    <message>
        <source>Measure</source>
        <extracomment>Keyboard shortcut list: turn the measuring tool on or off. Use the same word as the Measure action, the verb or the word for measuring.</extracomment>
        <translation>Измерете</translation>
    </message>
    <message>
        <source>Fill forms</source>
        <extracomment>Keyboard shortcut list: turn the form filling tool on or off.</extracomment>
        <translation>Попълнете формуляри</translation>
    </message>
    <message>
        <source>Print</source>
        <extracomment>Keyboard shortcut list (verb): print the document.</extracomment>
        <translation>Печат</translation>
    </message>
    <message>
        <source>Full screen</source>
        <translation>Цял екран</translation>
    </message>
    <message>
        <source>Settings</source>
        <translation>Настройки</translation>
    </message>
    <message>
        <source>General</source>
        <extracomment>Settings page title: general settings.</extracomment>
        <translation>Общи</translation>
    </message>
    <message>
        <source>Appearance</source>
        <translation>Външен вид</translation>
    </message>
    <message>
        <source>Viewing</source>
        <extracomment>Settings page title: how documents are shown (zoom, scrolling).</extracomment>
        <translation>Разглеждане</translation>
    </message>
    <message>
        <source>Annotations</source>
        <extracomment>Settings page title: highlights and comments added to documents.</extracomment>
        <translation>анотации</translation>
    </message>
    <message>
        <source>OCR</source>
        <extracomment>Settings page title. OCR is optical character recognition (text from images).</extracomment>
        <translation>OCR</translation>
    </message>
    <message>
        <source>Measuring</source>
        <extracomment>Settings page title: the measuring tool.</extracomment>
        <translation>Измерване</translation>
    </message>
    <message>
        <source>Forms</source>
        <extracomment>Settings page title: fillable PDF forms.</extracomment>
        <translation>формуляри</translation>
    </message>
    <message>
        <source>Keyboard shortcuts</source>
        <translation>Клавишни комбинации</translation>
    </message>
    <message>
        <source>About</source>
        <translation>Относно</translation>
    </message>
    <message>
        <source>Language</source>
        <extracomment>Group title: the language of Mervin&apos;s own text.</extracomment>
        <translation>език</translation>
    </message>
    <message>
        <source>Display language:</source>
        <translation>Език на дисплея:</translation>
    </message>
    <message>
        <source>Mervin will restart.</source>
        <translation>Мервин ще рестартира.</translation>
    </message>
    <message>
        <source>Opening files</source>
        <translation>Отваряне на файлове</translation>
    </message>
    <message>
        <source>New tab in current window</source>
        <translation>Нов раздел в текущия прозорец</translation>
    </message>
    <message>
        <source>New window</source>
        <comment>where a PDF opens</comment>
        <extracomment>Where a PDF opens: in a new window, not a new tab.</extracomment>
        <translation>Нов прозорец</translation>
    </message>
    <message>
        <source>When opening a PDF:</source>
        <translation>Когато отваряте PDF:</translation>
    </message>
    <message>
        <source>Reopen my tabs when Mervin starts</source>
        <translation>Отвори отново разделите ми, когато Мервин започне</translation>
    </message>
    <message>
        <source>Memory and tray</source>
        <translation>Памет и тава</translation>
    </message>
    <message>
        <source>Unload inactive documents after (minutes)</source>
        <extracomment>Label before a field for a number of minutes, followed by a &quot;Never&quot; checkbox.</extracomment>
        <translation>Разтоварване на неактивни документи от паметта след (минути)</translation>
    </message>
    <message>
        <source>Never</source>
        <extracomment>Checkbox after the minutes field: never unload inactive documents.</extracomment>
        <translation>Никога</translation>
    </message>
    <message>
        <source>Close to tray</source>
        <extracomment>Checkbox: closing the window keeps Mervin running in the system tray instead of quitting.</extracomment>
        <translation>Затваряне в системната област</translation>
    </message>
    <message>
        <source>Keep Mervin running when you close the window. Use Quit Mervin from the tray menu to exit.</source>
        <extracomment>Quit Mervin is the tray menu item of that name.</extracomment>
        <translation>Mervin продължава да работи след затваряне на прозореца. За изход изберете Изход от Mervin в менюто на системната област.</translation>
    </message>
    <message>
        <source>Recent files</source>
        <translation>Последни файлове</translation>
    </message>
    <message>
        <source>Recent files shown:</source>
        <extracomment>Label before a number: how many recent files the Recent page lists.</extracomment>
        <translation>Показани скорошни файлове:</translation>
    </message>
    <message>
        <source>Recent history kept:</source>
        <extracomment>Label before a number: how many recent files Mervin remembers.</extracomment>
        <translation>Пазена скорошна история:</translation>
    </message>
    <message>
        <source>Names</source>
        <extracomment>Where the Recent page&apos;s search looks first: in file names.</extracomment>
        <translation>имена</translation>
    </message>
    <message>
        <source>Contents</source>
        <extracomment>Where the Recent page&apos;s search looks first: in the text inside the files.</extracomment>
        <translation>Съдържание</translation>
    </message>
    <message>
        <source>All (names, then contents)</source>
        <translation>Всички (имена, след това съдържание)</translation>
    </message>
    <message>
        <source>Default search:</source>
        <translation>Търсене по подразбиране:</translation>
    </message>
    <message>
        <source>Keep removed files in list</source>
        <translation>Съхранявайте премахнатите файлове в списъка</translation>
    </message>
    <message>
        <source>Updates</source>
        <translation>Актуализации</translation>
    </message>
    <message>
        <source>Update automatically at start (every 30 days)</source>
        <extracomment>Settings checkbox. At startup, if 30 days have passed since the last completed check, download and install any new release automatically, then restart the app.</extracomment>
        <translation>Автоматично обновяване при стартиране (на всеки 30 дни)</translation>
    </message>
    <message>
        <source>This copy can&apos;t update itself. Check for Updates shows where to download a new version.</source>
        <extracomment>Check for Updates is the button below.</extracomment>
        <translation>Това копие не може да се актуализира само. Проверката за актуализации показва къде да изтеглите нова версия.</translation>
    </message>
    <message>
        <source>Check for Updates</source>
        <translation>Проверете за актуализации</translation>
    </message>
    <message>
        <source>System integration</source>
        <translation>Системна интеграция</translation>
    </message>
    <message>
        <source>Set as Default PDF App</source>
        <translation>Задайте като стандартно PDF приложение</translation>
    </message>
    <message>
        <source>Disabled while running with --profile</source>
        <extracomment>--profile is a command-line option; keep it as it is.</extracomment>
        <translation>Деактивирано, докато работи с --profile</translation>
    </message>
    <message>
        <source>Mervin PDF</source>
        <translation>Mervin PDF</translation>
    </message>
    <message>
        <source>Mervin couldn&apos;t set itself as your default PDF viewer automatically.

Open your system&apos;s Settings → Default Applications (or right-click a PDF → Open With) and choose Mervin PDF for PDF files.</source>
        <translation>Mervin не можа автоматично да се зададе като програма за преглед на PDF файлове по подразбиране.

Отворете Настройки → Приложения по подразбиране на вашата система (или щракнете с десния бутон върху PDF → Отваряне с) и изберете Mervin PDF за PDF файлове.</translation>
    </message>
    <message>
        <source>Application</source>
        <extracomment>Group title: the look of Mervin&apos;s windows, as opposed to the document pages.</extracomment>
        <translation>Приложение</translation>
    </message>
    <message>
        <source>Choose accent colour</source>
        <translation>Изберете цвят на акцента</translation>
    </message>
    <message>
        <source>Reset</source>
        <extracomment>Button (verb): go back to the default accent colour.</extracomment>
        <translation>Нулиране</translation>
    </message>
    <message>
        <source>Accent colour:</source>
        <translation>Цвят на акцента:</translation>
    </message>
    <message>
        <source>Use the system accent colour</source>
        <translation>Използвайте системния цвят на акцента</translation>
    </message>
    <message>
        <source>Document</source>
        <extracomment>Group title: how document pages are coloured.</extracomment>
        <translation>Документ</translation>
    </message>
    <message>
        <source>Comfort darkens the page and keeps photos readable.</source>
        <extracomment>Comfort is the name of a document theme above.</extracomment>
        <translation>Комфорт затъмнява страницата и запазва снимките четими.</translation>
    </message>
    <message>
        <source>Fit Width</source>
        <extracomment>Default zoom choice: scale the page so its width fits the window.</extracomment>
        <translation>Побери ширината</translation>
    </message>
    <message>
        <source>Fit Page</source>
        <extracomment>Default zoom choice: scale the page so the whole page fits the window.</extracomment>
        <translation>Побери страницата</translation>
    </message>
    <message>
        <source>Default zoom:</source>
        <translation>Увеличение по подразбиране:</translation>
    </message>
    <message>
        <source>Continuous scroll</source>
        <translation>Непрекъснато превъртане</translation>
    </message>
    <message>
        <source>Single page</source>
        <translation>Единична страница</translation>
    </message>
    <message>
        <source>Default scrolling:</source>
        <translation>Превъртане по подразбиране:</translation>
    </message>
    <message>
        <source>Show pages as two-page spreads</source>
        <translation>Показване на страници като разгъване на две страници</translation>
    </message>
    <message>
        <source>Default highlight colour:</source>
        <translation>Цвят за осветяване по подразбиране:</translation>
    </message>
    <message>
        <source>Colour applied to new highlights and comments</source>
        <translation>Цвят, приложен към нови акценти и коментари</translation>
    </message>
    <message>
        <source>Author name:</source>
        <translation>Име на автора:</translation>
    </message>
    <message>
        <source>Default language:</source>
        <extracomment>The language OCR reads text in unless the user picks another.</extracomment>
        <translation>Език по подразбиране:</translation>
    </message>
    <message>
        <source>Installed languages</source>
        <translation>Инсталирани езици</translation>
    </message>
    <message>
        <source>Add Languages...</source>
        <translation>Добавяне на езици...</translation>
    </message>
    <message>
        <source>Open Folder</source>
        <extracomment>Button: open the folder that holds the OCR language files.</extracomment>
        <translation>Отворете папката</translation>
    </message>
    <message>
        <source>Snap to vertices &amp;&amp; edges</source>
        <extracomment>&quot;&amp;&amp;&quot; shows as a single &quot;&amp;&quot;.</extracomment>
        <translation>Прилепване към върхове &amp;&amp; ръбове</translation>
    </message>
    <message>
        <source>Snap endpoints to the drawing&apos;s lines for precise picks on CAD geometry</source>
        <translation>Прилепете крайните точки към линиите на чертежа за прецизен избор на CAD геометрия</translation>
    </message>
    <message>
        <source>Distance</source>
        <extracomment>Measurement type (noun): a straight distance between two points.</extracomment>
        <translation>Разстояние</translation>
    </message>
    <message>
        <source>Path</source>
        <extracomment>Measurement type (noun): the length of a line with several segments.</extracomment>
        <translation>Дължина на полилиния</translation>
    </message>
    <message>
        <source>Area</source>
        <extracomment>Measurement type (noun): the area of a polygon.</extracomment>
        <translation>Площ</translation>
    </message>
    <message>
        <source>Angle</source>
        <extracomment>Measurement type (noun): the angle between two lines.</extracomment>
        <translation>Ъгъл</translation>
    </message>
    <message>
        <source>Default type:</source>
        <extracomment>The measurement type a new measuring tool starts with.</extracomment>
        <translation>Тип по подразбиране:</translation>
    </message>
    <message>
        <source>Default unit:</source>
        <translation>Единица по подразбиране:</translation>
    </message>
    <message>
        <source>Decimal places:</source>
        <translation>Знаци след десетичната запетая:</translation>
    </message>
    <message>
        <source>Line width:</source>
        <translation>Ширина на линията:</translation>
    </message>
    <message>
        <source>Open documents with forms in Fill Forms mode</source>
        <translation>Отваряне на документи с формуляри в режим на попълване на формуляри</translation>
    </message>
    <message>
        <source>Automatically enter Fill Forms mode when a PDF has fillable fields</source>
        <translation>Автоматично влизане в режим на попълване на формуляри, когато PDF има полета за попълване</translation>
    </message>
    <message>
        <source>Highlight fillable form fields</source>
        <translation>Маркирайте полетата на формуляра за попълване</translation>
    </message>
    <message>
        <source>Tint fillable form fields while the Fill Forms tool is active</source>
        <translation>Оцветете попълваемите полета на формуляри, докато инструментът за попълване на формуляри е активен</translation>
    </message>
    <message>
        <source>Version %1</source>
        <extracomment>%1 is the version number, such as 1.64.13.</extracomment>
        <translation>Версия %1</translation>
    </message>
    <message>
        <source>&lt;p&gt;Mervin PDF is free software under the GNU AGPL v3, with no warranty.&lt;/p&gt;&lt;p&gt;&lt;b&gt;Open-source components&lt;/b&gt;&lt;/p&gt;&lt;ul&gt;&lt;li&gt;&lt;b&gt;Qt 6&lt;/b&gt;: GNU LGPL v3 (dynamically linked; libraries may be replaced).&lt;/li&gt;&lt;li&gt;&lt;b&gt;MuPDF&lt;/b&gt; (Artifex): GNU AGPL v3 or commercial. Rendering, text, OCR.&lt;/li&gt;&lt;li&gt;&lt;b&gt;qpdf&lt;/b&gt;: Apache 2.0. Security &amp;amp; page operations.&lt;/li&gt;&lt;li&gt;&lt;b&gt;Tesseract&lt;/b&gt;: Apache 2.0, and &lt;b&gt;Leptonica&lt;/b&gt;: BSD. Selection OCR.&lt;/li&gt;&lt;li&gt;&lt;b&gt;toml++&lt;/b&gt;: MIT. Configuration file.&lt;/li&gt;&lt;li&gt;Bundled by MuPDF: FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, jbig2dec (AGPL), zlib.&lt;/li&gt;&lt;/ul&gt;&lt;p&gt;See &lt;i&gt;THIRD_PARTY_LICENSES.md&lt;/i&gt; for full licence texts.&lt;/p&gt;</source>
        <extracomment>Licence notice. Keep the HTML tags, the component and licence names, and THIRD_PARTY_LICENSES.md as they are.</extracomment>
        <translation>&lt;p&gt;Mervin PDF е свободен софтуер под лиценза GNU AGPL v3, без гаранция.&lt;/p&gt;&lt;p&gt;&lt;b&gt;Компоненти с отворен код&lt;/b&gt;&lt;/p&gt;&lt;ul&gt;&lt;li&gt;&lt;b&gt;Qt 6&lt;/b&gt;: GNU LGPL v3 (динамично свързан; библиотеките могат да бъдат заменени).&lt;/li&gt;&lt;li&gt;&lt;b&gt;MuPDF&lt;/b&gt; (Artifex): GNU AGPL v3 или търговски. Изобразяване, текст, OCR.&lt;/li&gt;&lt;li&gt;&lt;b&gt;qpdf&lt;/b&gt;: Apache 2.0. Сигурност и операции със страници.&lt;/li&gt;&lt;li&gt;&lt;b&gt;Tesseract&lt;/b&gt;: Apache 2.0 и &lt;b&gt;Leptonica&lt;/b&gt;: BSD. Избор OCR.&lt;/li&gt;&lt;li&gt;&lt;b&gt;toml++&lt;/b&gt;: MIT. Конфигурационен файл.&lt;/li&gt;&lt;li&gt;Пакет от MuPDF: FreeType, HarfBuzz, libjpeg-turbo, OpenJPEG, jbig2dec (AGPL), zlib.&lt;/li&gt;&lt;/ul&gt;&lt;p&gt;Вижте &lt;i&gt;THIRD_PARTY_LICENSES.md&lt;/i&gt; за пълни лицензни текстове.&lt;/p&gt;</translation>
    </message>
    <message>
        <source>Enter 1 to %1 whole minutes, or check Never.</source>
        <extracomment>%1 is the largest number of minutes allowed. Never is the checkbox of that name.</extracomment>
        <translation>Въведете от 1 до %1 цели минути или отметнете Никога.</translation>
    </message>
    <message>
        <source>Keep documents loaded, including in the tray.</source>
        <translation>Запазвай документите в паметта, включително при работа в системната област.</translation>
    </message>
    <message>
        <source>Couldn&apos;t save the settings.

%1</source>
        <translation>Настройките не можаха да бъдат запазени.

%1</translation>
    </message>
    <message>
        <source>Never checked</source>
        <translation>Никога не е проверяван</translation>
    </message>
    <message>
        <source>Last checked %1</source>
        <extracomment>%1 is the date of the last update check, such as &quot;2 October 2026&quot;.</extracomment>
        <translation>Последна проверка %1</translation>
    </message>
    <message>
        <source>None</source>
        <extracomment>Shown in the OCR language picker when no language is installed.</extracomment>
        <translation>Няма</translation>
    </message>
    <message>
        <source>OCR needs at least one language</source>
        <translation>OCR се нуждае от поне един език</translation>
    </message>
    <message>
        <source>Remove %1</source>
        <extracomment>Tooltip of a delete button. %1 is the name of an OCR language, such as Swedish.</extracomment>
        <translation>Премахване на %1</translation>
    </message>
    <message>
        <source>No OCR languages installed</source>
        <translation>Няма инсталирани OCR езици</translation>
    </message>
    <message>
        <source>Remove OCR Language</source>
        <translation>Премахване на OCR език</translation>
    </message>
    <message>
        <source>Remove %1? You can download it again with Add Languages.</source>
        <extracomment>%1 is the name of an OCR language. Add Languages is the button of that name.</extracomment>
        <translation>Премахване на %1? Можете да го изтеглите отново с Добавяне на езици.</translation>
    </message>
    <message>
        <source>Could not remove %1 from the OCR language folder.</source>
        <extracomment>%1 is the name of an OCR language.</extracomment>
        <translation>Не може да се премахне %1 от езиковата папка за OCR.</translation>
    </message>
</context>
<context>
    <name>UiLanguage</name>
    <message>
        <source>English</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>английски</translation>
    </message>
    <message>
        <source>Arabic</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>арабски</translation>
    </message>
    <message>
        <source>Azerbaijani</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>азербайджански</translation>
    </message>
    <message>
        <source>Belarusian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>белоруски</translation>
    </message>
    <message>
        <source>Bulgarian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>български</translation>
    </message>
    <message>
        <source>Bosnian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>босненски</translation>
    </message>
    <message>
        <source>Catalan</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>каталонски</translation>
    </message>
    <message>
        <source>Montenegrin</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>черногорски</translation>
    </message>
    <message>
        <source>Czech</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>чешки</translation>
    </message>
    <message>
        <source>Danish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>датски</translation>
    </message>
    <message>
        <source>German</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>немски</translation>
    </message>
    <message>
        <source>Greek</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>гръцки</translation>
    </message>
    <message>
        <source>Spanish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>испански</translation>
    </message>
    <message>
        <source>Estonian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>естонски</translation>
    </message>
    <message>
        <source>Finnish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>финландски</translation>
    </message>
    <message>
        <source>French</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>френски</translation>
    </message>
    <message>
        <source>Irish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>ирландски</translation>
    </message>
    <message>
        <source>Hindi</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>хинди</translation>
    </message>
    <message>
        <source>Croatian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>хърватски</translation>
    </message>
    <message>
        <source>Hungarian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>унгарски</translation>
    </message>
    <message>
        <source>Armenian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>арменски</translation>
    </message>
    <message>
        <source>Indonesian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>индонезийски</translation>
    </message>
    <message>
        <source>Icelandic</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>исландски</translation>
    </message>
    <message>
        <source>Italian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>италиански</translation>
    </message>
    <message>
        <source>Japanese</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>японски</translation>
    </message>
    <message>
        <source>Georgian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>грузински</translation>
    </message>
    <message>
        <source>Kazakh</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>казахски</translation>
    </message>
    <message>
        <source>Korean</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>корейски</translation>
    </message>
    <message>
        <source>Luxembourgish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>люксембургски</translation>
    </message>
    <message>
        <source>Lithuanian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>литовски</translation>
    </message>
    <message>
        <source>Latvian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>латвийски</translation>
    </message>
    <message>
        <source>Macedonian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>македонски</translation>
    </message>
    <message>
        <source>Maltese</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>малтийски</translation>
    </message>
    <message>
        <source>Norwegian Bokmål</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>норвежки букмол</translation>
    </message>
    <message>
        <source>Dutch</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>холандски</translation>
    </message>
    <message>
        <source>Norwegian Nynorsk</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>норвежки нюнорск</translation>
    </message>
    <message>
        <source>Polish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>полски</translation>
    </message>
    <message>
        <source>Portuguese, Brazil</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>Португалски, Бразилия</translation>
    </message>
    <message>
        <source>Portuguese, Portugal</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>португалски, Португалия</translation>
    </message>
    <message>
        <source>Romansh</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>романш</translation>
    </message>
    <message>
        <source>Romanian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>румънски</translation>
    </message>
    <message>
        <source>Russian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>руски</translation>
    </message>
    <message>
        <source>Slovak</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>словашки</translation>
    </message>
    <message>
        <source>Slovenian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>словенски</translation>
    </message>
    <message>
        <source>Albanian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>албански</translation>
    </message>
    <message>
        <source>Serbian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>сръбски</translation>
    </message>
    <message>
        <source>Swedish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>шведски</translation>
    </message>
    <message>
        <source>Thai</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>тайландски</translation>
    </message>
    <message>
        <source>Turkish</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>турски</translation>
    </message>
    <message>
        <source>Ukrainian</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>украински</translation>
    </message>
    <message>
        <source>Vietnamese</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>виетнамски</translation>
    </message>
    <message>
        <source>Chinese, Simplified</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>китайски, опростен</translation>
    </message>
    <message>
        <source>Chinese, Traditional</source>
        <extracomment>Language name. The language picker&apos;s search finds a language by this name too.</extracomment>
        <translation>китайски, традиционен</translation>
    </message>
</context>
<context>
    <name>mervin::AnnotPanel</name>
    <message>
        <source>Comment</source>
        <extracomment>Title of the comment tool panel, and its button that drops a sticky-note comment on the page.</extracomment>
        <translation>Коментар</translation>
    </message>
    <message>
        <source>Close comment tool</source>
        <translation>Затворете инструмента за коментари</translation>
    </message>
    <message>
        <source>Select</source>
        <extracomment>Mode button (a verb). Clicks select text and make no annotation.</extracomment>
        <translation>Изберете</translation>
    </message>
    <message>
        <source>Pointer: select and copy text (no annotation)</source>
        <translation>Показалец: изберете и копирайте текст (без анотация)</translation>
    </message>
    <message>
        <source>Click the page to drop a sticky-note comment</source>
        <translation>Щракнете върху страницата, за да пуснете коментар с лепкава бележка</translation>
    </message>
    <message>
        <source>Highlight</source>
        <extracomment>Markup style button (a verb).</extracomment>
        <translation>Маркирайте</translation>
    </message>
    <message>
        <source>Highlight the selected text</source>
        <translation>Маркирайте избрания текст</translation>
    </message>
    <message>
        <source>Underline</source>
        <extracomment>Markup style button (a verb).</extracomment>
        <translation>Подчертайте</translation>
    </message>
    <message>
        <source>Underline the selected text</source>
        <translation>Подчертайте избрания текст</translation>
    </message>
    <message>
        <source>Strike out</source>
        <extracomment>Markup style button (a verb).</extracomment>
        <translation>Зачеркни</translation>
    </message>
    <message>
        <source>Strike out the selected text</source>
        <translation>Зачеркни избрания текст</translation>
    </message>
</context>
<context>
    <name>mervin::AnnotPopup</name>
    <message>
        <source>Close</source>
        <extracomment>Tooltip of the button that closes the annotation card (verb).</extracomment>
        <translation>затвори</translation>
    </message>
    <message>
        <source>Add a comment…</source>
        <translation>Добавете коментар...</translation>
    </message>
    <message>
        <source>Delete annotation</source>
        <translation>Изтриване на анотация</translation>
    </message>
    <message>
        <source>Annotation</source>
        <extracomment>Header of an annotation&apos;s card when the annotation names no author.</extracomment>
        <translation>Анотация</translation>
    </message>
    <message>
        <source>%1  ·  %2</source>
        <extracomment>Header of an annotation&apos;s card. %1 is the author (or &quot;Annotation&quot;), %2 the date it was last changed.</extracomment>
        <translation>%1  ·  %2</translation>
    </message>
</context>
<context>
    <name>mervin::CalibrationDialog</name>
    <message>
        <source>Calibrate</source>
        <extracomment>Window title (verb): set the drawing scale from a line of known length.</extracomment>
        <translation>Калибриране</translation>
    </message>
    <message>
        <source>Enter the real length of the line you drew to set the drawing scale.</source>
        <translation>Въведете реалната дължина на линията, която сте начертали, за да зададете мащаба на чертежа.</translation>
    </message>
    <message>
        <source>Known length:</source>
        <extracomment>Label before a number field and a unit picker: the real length of the line the user drew.</extracomment>
        <translation>Известна дължина:</translation>
    </message>
    <message>
        <source>Set Scale</source>
        <extracomment>Window title (verb): type the drawing scale as a ratio.</extracomment>
        <translation>Задай мащаб</translation>
    </message>
    <message>
        <source>Set the drawing scale by typing a scale ratio.</source>
        <translation>Задайте мащаба на чертежа, като въведете съотношение на мащаба.</translation>
    </message>
    <message>
        <source>Scale ratio 1 :</source>
        <extracomment>Label directly before a number field N. Together they read as the ratio &quot;1 : N&quot;, such as &quot;Scale ratio 1 : 100&quot; (1 unit on paper is N real units). Keep &quot;1 :&quot; at the end so the number follows it.</extracomment>
        <translation>Мащаб 1 :</translation>
    </message>
</context>
<context>
    <name>mervin::CommentsSidebar</name>
    <message>
        <source>Highlight</source>
        <extracomment>Noun naming an annotation kind, highlighted text.</extracomment>
        <translation>Маркиране</translation>
    </message>
    <message>
        <source>Underline</source>
        <extracomment>Noun naming an annotation kind, underlined text.</extracomment>
        <translation>Подчертаване</translation>
    </message>
    <message>
        <source>Strike-out</source>
        <extracomment>Noun naming an annotation kind, struck-out text.</extracomment>
        <translation>Зачеркване</translation>
    </message>
    <message>
        <source>Note</source>
        <extracomment>Noun naming an annotation kind, a sticky note.</extracomment>
        <translation>Забележка</translation>
    </message>
    <message>
        <source>Annotation</source>
        <translation>Анотация</translation>
    </message>
    <message>
        <source>No annotations yet.

Use the Highlight or Comment tool to add some.</source>
        <extracomment>&quot;Comment&quot; names the toolbar tool for highlights and sticky notes, and &quot;Highlight&quot; a style in it. Use their translations here.</extracomment>
        <translation>Все още няма анотации.

Използвайте инструмента за подчертаване или коментар, за да добавите някои.</translation>
    </message>
    <message>
        <source>p.%1  %2</source>
        <extracomment>A row in the comments list. &quot;p.&quot; is short for page. %1 is the page number, %2 the comment text.</extracomment>
        <translation>стр.%1 %2</translation>
    </message>
    <message>
        <source>p.%1  %2 · %3</source>
        <extracomment>A row in the comments list. &quot;p.&quot; is short for page. %1 is the page number, %2 the comment&apos;s author, %3 the comment text.</extracomment>
        <translation>стр.%1 %2 · %3</translation>
    </message>
</context>
<context>
    <name>mervin::Document</name>
    <message>
        <source>Could not write the PDF.</source>
        <translation>PDF файлът не можа да се напише.</translation>
    </message>
</context>
<context>
    <name>mervin::DocumentOutput</name>
    <message>
        <source>Could not create a temporary document.</source>
        <translation>Не можа да се създаде временен документ.</translation>
    </message>
</context>
<context>
    <name>mervin::DocumentThemePicker</name>
    <message>
        <source>Document theme</source>
        <translation>Тема на документа</translation>
    </message>
    <message>
        <source>Traditional</source>
        <extracomment>Document theme name: pages as authored, on white paper.</extracomment>
        <translation>Традиционен</translation>
    </message>
    <message>
        <source>Comfort</source>
        <extracomment>Document theme name: dark grey pages that are easy on the eyes, with readable photos.</extracomment>
        <translation>Комфорт</translation>
    </message>
    <message>
        <source>Inverted</source>
        <extracomment>Document theme name: black pages with all colours inverted.</extracomment>
        <translation>Обърнат</translation>
    </message>
    <message>
        <source>White paper, dark text and pictures in their original colours</source>
        <translation>Бяла хартия, тъмен текст и снимки в оригиналните им цветове</translation>
    </message>
    <message>
        <source>Dark grey paper, light text and readable photos</source>
        <translation>Тъмно сива хартия, светъл текст и четливи снимки</translation>
    </message>
    <message>
        <source>Black paper, light text and inverted picture colours</source>
        <translation>Черна хартия, светъл текст и обърнати цветове на картината</translation>
    </message>
    <message>
        <source>Follow UI theme</source>
        <extracomment>Document theme choice: dark pages with a dark UI theme, light pages with a light one.</extracomment>
        <translation>Следвайте темата на потребителския интерфейс</translation>
    </message>
</context>
<context>
    <name>mervin::ExportMeasureDialog</name>
    <message>
        <source>Export with Measurements</source>
        <translation>Експортиране с измервания</translation>
    </message>
    <message>
        <source>This document contains Mervin PDF measurements.
When you export a copy, the measurements are added to the PDF.
These exported measurements are non-editable.
The exported file is viewable in any PDF viewer.</source>
        <extracomment>One sentence per line. Keep the line breaks.</extracomment>
        <translation>Този документ съдържа измервания на Mervin PDF.
Когато експортирате копие, измерванията се добавят към PDF файла.
Тези експортирани измервания не могат да се редактират.
Експортираният файл може да се види във всяка PDF програма за преглед.</translation>
    </message>
</context>
<context>
    <name>mervin::ExtractDialog</name>
    <message>
        <source>Delete pages from %1</source>
        <extracomment>Title of the Delete pages window. %1 is the document&apos;s file name, like &quot;report.pdf&quot;. The window writes a new file without the chosen pages.</extracomment>
        <translation>Изтриване на страници от %1</translation>
    </message>
    <message>
        <source>Extract Pages</source>
        <translation>Извличане на страници</translation>
    </message>
    <message>
        <source>Pages</source>
        <extracomment>Column caption over the page range fields. The next label, &quot;of %1&quot;, follows it in a lighter style and reads &quot;Pages of 31&quot;.</extracomment>
        <translation>Страници</translation>
    </message>
    <message>
        <source>&amp;Add Range</source>
        <extracomment>Button: adds an empty row below the selected one.</extracomment>
        <translation>&amp;Добавете диапазон</translation>
    </message>
    <message>
        <source>Move &amp;Up</source>
        <translation>&amp;Преместване нагоре</translation>
    </message>
    <message>
        <source>Move &amp;Down</source>
        <translation>&amp;Преместване надолу</translation>
    </message>
    <message>
        <source>&amp;Remove</source>
        <extracomment>Button: removes the selected row.</extracomment>
        <translation>&amp;Премахнете</translation>
    </message>
    <message>
        <source>Delete Page</source>
        <extracomment>Context menu item of a page in the Delete pages strip. It marks the page to be left out of the new file.</extracomment>
        <translation>Изтрий страницата</translation>
    </message>
    <message>
        <source>Keep Page</source>
        <extracomment>Context menu item of a page marked for deletion in the Delete pages strip. It takes the mark away, so the new file keeps the page.</extracomment>
        <translation>Остави страницата</translation>
    </message>
    <message>
        <source>Remove</source>
        <extracomment>Context menu item of a page in the strip: takes it out of the extract.</extracomment>
        <translation>Премахнете</translation>
    </message>
    <message>
        <source>Pass&amp;word:</source>
        <translation>&amp;Парола:</translation>
    </message>
    <message>
        <source>This PDF is encrypted</source>
        <translation>Този PDF файл е шифрован</translation>
    </message>
    <message>
        <source>&amp;Save as:</source>
        <translation>&amp;Запиши като:</translation>
    </message>
    <message>
        <source>&amp;Browse…</source>
        <translation>&amp;Преглед...</translation>
    </message>
    <message>
        <source>&amp;Open when done</source>
        <translation>&amp;Отворете, когато сте готови</translation>
    </message>
    <message>
        <source>Delete pages</source>
        <extracomment>Button: writes a new file without the listed pages. The original file keeps every page.</extracomment>
        <translation>Изтрий страниците</translation>
    </message>
    <message>
        <source>Extract</source>
        <extracomment>Button: writes the extracted pages to a new file.</extracomment>
        <translation>Извлечи</translation>
    </message>
    <message>
        <source>e.g. 5-7</source>
        <translation>напр. 5-7</translation>
    </message>
    <message>
        <source>One page or range, like 5 or 5-7.</source>
        <translation>Една страница или диапазон, като 5 или 5-7.</translation>
    </message>
    <message>
        <source>Show Pages %1</source>
        <extracomment>Context menu item of a tile standing in for hidden pages. %1 is their range, like &quot;14-30&quot;.</extracomment>
        <translation>Показване на страници %1</translation>
    </message>
    <message>
        <source>of %1</source>
        <extracomment>Follows the &quot;Pages&quot; column caption, reading &quot;Pages of 31&quot;. %1 is the source document&apos;s page count.</extracomment>
        <translation>от %1</translation>
    </message>
    <message>
        <source>%1 Unsaved changes are not included.</source>
        <extracomment>%1 is the result sentence, like &quot;Result: 4 pages, in the order shown.&quot;</extracomment>
        <translation>%1 Незапазените промени не са включени.</translation>
    </message>
    <message>
        <source>This PDF cannot be read for deleting pages.</source>
        <extracomment>The viewer shows the document, but qpdf, which writes the new file, cannot read it.</extracomment>
        <translation>Този PDF файл не може да бъде прочетен за изтриване на страници.</translation>
    </message>
    <message>
        <source>This PDF cannot be read for extraction.</source>
        <translation>Този PDF файл не може да бъде прочетен за извличане.</translation>
    </message>
    <message>
        <source>Enter the password for this PDF.</source>
        <translation>Въведете паролата за този PDF файл.</translation>
    </message>
    <message>
        <source>That password is not correct.</source>
        <translation>Тази парола не е правилна.</translation>
    </message>
    <message>
        <source>Save Edited Copy</source>
        <extracomment>Title of the file picker for the new file Delete pages writes.</extracomment>
        <translation>Запазване на редактираното копие</translation>
    </message>
    <message>
        <source>Save Extracted Pages</source>
        <translation>Запазване на извлечените страници</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf)</source>
        <extracomment>File type filter. Keep &quot;(*.pdf)&quot;.</extracomment>
        <translation>PDF документи (*.pdf)</translation>
    </message>
    <message>
        <source>The page count changed. Check the ranges and try again.</source>
        <extracomment>The source file changed its number of pages after the dialog&apos;s initial probe. The user&apos;s ranges remain and need another review.</extracomment>
        <translation>Броят на страниците е променен. Проверете диапазоните и опитайте отново.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; already exists. Replace it?</source>
        <extracomment>%1 is a file path.</extracomment>
        <translation>„%1“ вече съществува. Да го замените?</translation>
    </message>
    <message>
        <source>Could not write &quot;%1&quot;. If another program has it open, close it and press Delete pages again, or choose another name.</source>
        <extracomment>%1 is the new file&apos;s name. &quot;Delete pages&quot; is the button&apos;s text.</extracomment>
        <translation>Не може да се запише „%1“. Ако файлът е отворен в друга програма, затворете го и натиснете отново „Изтрий страниците“ или изберете друго име.</translation>
    </message>
    <message>
        <source>Could not write &quot;%1&quot;. If another program has it open, close it and press Extract again, or choose another name.</source>
        <extracomment>%1 is the new file&apos;s name.</extracomment>
        <translation>Не може да се напише &quot;%1&quot;. Ако друга програма го е отворила, затворете го и натиснете отново Извличане или изберете друго име.</translation>
    </message>
</context>
<context>
    <name>mervin::ExtractPlan</name>
    <message>
        <source>Enter a page or range, like 5-7.</source>
        <translation>Въведете страница или диапазон, като 5-7.</translation>
    </message>
    <message>
        <source>Result: 1 page.</source>
        <translation>Резултат: 1 страница.</translation>
    </message>
    <message>
        <source>Add a range of pages to extract.</source>
        <translation>Добавете набор от страници за извличане.</translation>
    </message>
    <message>
        <source>Row %1: %2</source>
        <extracomment>%1 is the row number, %2 the sentence saying what is wrong with it.</extracomment>
        <translation>Ред %1: %2</translation>
    </message>
    <message>
        <source>Page %1</source>
        <extracomment>Tooltip of a page in the extract strip: %1 is its page number.</extracomment>
        <translation>Страница %1</translation>
    </message>
    <message>
        <source>Page %1 becomes page %2 of the extract.</source>
        <extracomment>%1 is the page number in the source document, %2 its position in the new file.</extracomment>
        <translation>Страница %1 става страница %2 от извлечението.</translation>
    </message>
    <message numerus="yes">
        <source>Result: %n page(s), in the order shown.</source>
        <translation>
            <numerusform>Резултат: %n страница, в показания ред.</numerusform>
            <numerusform>Резултат: %n страници, в посочения ред.</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Result: %n of %1 page(s) remain (%2).</source>
        <extracomment>Result line of Delete pages, under the page strip. %n is how many pages the new file keeps, %1 the document&apos;s page count, %2 the pages that remain as ranges, like &quot;1, 3-4&quot;. In English the verb follows %n: &quot;1 of 6 pages remains&quot;, &quot;3 of 6 pages remain&quot;.</extracomment>
        <translation>
            <numerusform>Резултат: остава %n от %1 страници (%2).</numerusform>
            <numerusform>Резултат: остават %n от %1 страници (%2).</numerusform>
        </translation>
    </message>
    <message>
        <source>Click the pages to delete, or add a range.</source>
        <extracomment>Delete pages with no rows left. Clicking a page in the strip below marks it for deletion; &quot;add a range&quot; refers to the Add Range button.</extracomment>
        <translation>Кликнете върху страниците за изтриване или добавете диапазон.</translation>
    </message>
    <message>
        <source>Page %1 will be deleted. Click to keep it.</source>
        <extracomment>Tooltip of a page struck out in the Delete pages strip. %1 is its page number. The original file keeps it; only the new file leaves it out.</extracomment>
        <translation>Страница %1 ще бъде изтрита. Кликнете, за да я оставите.</translation>
    </message>
    <message>
        <source>Click to delete page %1.</source>
        <extracomment>Tooltip of a page in the Delete pages strip. %1 is its page number.</extracomment>
        <translation>Кликнете, за да изтриете страница %1.</translation>
    </message>
    <message numerus="yes">
        <source>Pages %1 (%n page(s)). Click to show them.</source>
        <extracomment>Tooltip of a tile standing in for hidden pages: %1 is their range, like &quot;14-30&quot;.</extracomment>
        <translation>
            <numerusform>Страници %1 (%n страница). Кликнете, за да ги покажете.</numerusform>
            <numerusform>Страници %1 (%n страници). Кликнете, за да ги покажете.</numerusform>
        </translation>
    </message>
    <message>
        <source>Choose where to save the edited copy.</source>
        <extracomment>Delete pages with an empty Save as field. The edited copy is the new file without the deleted pages.</extracomment>
        <translation>Изберете къде да запазите редактираното копие.</translation>
    </message>
    <message>
        <source>Choose where to save the extracted pages.</source>
        <translation>Изберете къде да запазите извлечените страници.</translation>
    </message>
    <message>
        <source>That is a folder. Add a file name.</source>
        <translation>Това е папка. Добавете име на файл.</translation>
    </message>
    <message>
        <source>That folder does not exist.</source>
        <translation>Тази папка не съществува.</translation>
    </message>
    <message>
        <source>The edited copy cannot replace the document it comes from.</source>
        <extracomment>Delete pages always writes a new file and never changes the original.</extracomment>
        <translation>Редактираното копие не може да замени документа, от който идва.</translation>
    </message>
    <message>
        <source>The extract cannot replace the document it comes from.</source>
        <translation>Извлечението не може да замени документа, от който идва.</translation>
    </message>
    <message>
        <source>That file is open in a tab. Choose another name.</source>
        <translation>Този файл е отворен в раздел. Изберете друго име.</translation>
    </message>
    <message numerus="yes">
        <source>Deleted %n page(s) in a copy saved as %1</source>
        <extracomment>Status bar message after Delete pages wrote its new file. %n is how many pages it left out, %1 the new file&apos;s name. The original is unchanged.</extracomment>
        <translation>
            <numerusform>Изтрита е %n страница в копие, запазено като %1</numerusform>
            <numerusform>Изтрити са %n страници в копие, запазено като %1</numerusform>
        </translation>
    </message>
    <message>
        <source>Extracted page %1 to %2</source>
        <extracomment>%1 is the page number, %2 the new file&apos;s name.</extracomment>
        <translation>Страница %1 е извлечена в %2</translation>
    </message>
    <message numerus="yes">
        <source>Extracted %n page(s) to %1</source>
        <extracomment>%1 is the new file&apos;s name.</extracomment>
        <translation>
            <numerusform>Извлечена е %n страница в %1</numerusform>
            <numerusform>Извлечени са %n страници в %1</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::ExtractStrip</name>
    <message>
        <source>Remove</source>
        <extracomment>Tooltip of the close button on a page in the strip: takes it out of the extract.</extracomment>
        <translation>Премахнете</translation>
    </message>
</context>
<context>
    <name>mervin::FileContextMenu</name>
    <message>
        <source>Copy file</source>
        <extracomment>Menu item that puts the file itself on the clipboard, to paste in a file manager.</extracomment>
        <translation>Копиране на файл</translation>
    </message>
    <message>
        <source>Open folder</source>
        <extracomment>Menu item that shows the folder holding the file.</extracomment>
        <translation>Отворете папката</translation>
    </message>
    <message>
        <source>Copy file path</source>
        <translation>Копирайте пътя на файла</translation>
    </message>
    <message>
        <source>Copy folder path</source>
        <translation>Копиране на пътя на папката</translation>
    </message>
    <message>
        <source>Could not copy &quot;%1&quot;: the file no longer exists.</source>
        <extracomment>%1 is the file&apos;s path.</extracomment>
        <translation>Не може да се копира &quot;%1&quot;: файлът вече не съществува.</translation>
    </message>
</context>
<context>
    <name>mervin::FindCard</name>
    <message>
        <source>Find in document</source>
        <translation>Намерете в документа</translation>
    </message>
    <message>
        <source>Previous match (Shift+Enter)</source>
        <extracomment>Go to the previous occurrence of the search text. Match means a search result.</extracomment>
        <translation>Предишно съвпадение (Shift+Enter)</translation>
    </message>
    <message>
        <source>Next match (Enter)</source>
        <extracomment>Go to the next occurrence of the search text. Match means a search result.</extracomment>
        <translation>Следващо съвпадение (Enter)</translation>
    </message>
    <message>
        <source>Match case</source>
        <extracomment>Search option. Distinguish uppercase and lowercase letters.</extracomment>
        <translation>Различавай главни и малки букви</translation>
    </message>
    <message>
        <source>Whole word</source>
        <translation>Цяла дума</translation>
    </message>
    <message>
        <source>Close (Esc)</source>
        <translation>Затваряне (Esc)</translation>
    </message>
    <message>
        <source>No results</source>
        <translation>Няма резултати</translation>
    </message>
    <message>
        <source>%1 of %2</source>
        <extracomment>Match counter in the find field. %1 is the current match, %2 the number of matches.</extracomment>
        <translation>%1 от %2</translation>
    </message>
</context>
<context>
    <name>mervin::FirstRunDialog</name>
    <message>
        <source>Welcome to Mervin PDF</source>
        <translation>Добре дошли в Mervin PDF</translation>
    </message>
    <message>
        <source>Display language:</source>
        <translation>Език на дисплея:</translation>
    </message>
    <message>
        <source>Display language</source>
        <extracomment>Accessible name of the picker for the language of Mervin&apos;s own text.</extracomment>
        <translation>Език на дисплея</translation>
    </message>
    <message>
        <source>Download OCR</source>
        <extracomment>First-run checkbox. Download the best OCR language models for the selected display language and OS language after Continue. OCR means text recognition.</extracomment>
        <translation>Изтегляне на OCR</translation>
    </message>
    <message>
        <source>Make Mervin PDF my default PDF viewer</source>
        <translation>Направете Mervin PDF моя програма за преглед на PDF файлове по подразбиране</translation>
    </message>
    <message>
        <source>Windows Settings opens so you can confirm.</source>
        <extracomment>Shown under the default PDF viewer checkbox. Windows Settings is the system app.</extracomment>
        <translation>Настройките на Windows се отварят, за да можете да потвърдите.</translation>
    </message>
    <message>
        <source>Continue</source>
        <translation>Продължи</translation>
    </message>
    <message>
        <source>Mervin couldn&apos;t set itself as your default PDF viewer automatically.

Open your system&apos;s Settings → Default Applications (or right-click a PDF → Open With) and choose Mervin PDF for PDF files.</source>
        <translation>Mervin не можа автоматично да се зададе като програма за преглед на PDF файлове по подразбиране.

Отворете Настройки → Приложения по подразбиране на вашата система (или щракнете с десния бутон върху PDF → Отваряне с) и изберете Mervin PDF за PDF файлове.</translation>
    </message>
</context>
<context>
    <name>mervin::LanguageCombo</name>
    <message>
        <source>Search languages</source>
        <translation>Търсене на езици</translation>
    </message>
    <message>
        <source>Languages</source>
        <extracomment>Accessible name of the list of UI languages below the search field.</extracomment>
        <translation>Езици</translation>
    </message>
</context>
<context>
    <name>mervin::ManageLanguagesDialog</name>
    <message>
        <source>Manage OCR Languages</source>
        <translation>Управление на OCR езици</translation>
    </message>
    <message>
        <source>Default language</source>
        <extracomment>Label for the OCR language that new OCR selections start with.</extracomment>
        <translation>Език по подразбиране</translation>
    </message>
    <message>
        <source>Default OCR language</source>
        <translation>OCR език по подразбиране</translation>
    </message>
    <message>
        <source>Available - best models</source>
        <extracomment>Column heading. The list holds Tesseract&apos;s best-quality OCR models.</extracomment>
        <translation>Налични - най-добрите модели</translation>
    </message>
    <message>
        <source>Search languages or codes</source>
        <translation>Търсете езици или кодове</translation>
    </message>
    <message>
        <source>Search available OCR languages</source>
        <translation>Търсене на налични OCR езици</translation>
    </message>
    <message>
        <source>Loading best-quality language models...</source>
        <translation>Зареждат се езикови модели с най-добро качество...</translation>
    </message>
    <message>
        <source>Language catalog unavailable</source>
        <translation>Езиковият каталог не е наличен</translation>
    </message>
    <message>
        <source>Check your connection, then reopen this dialog. %1</source>
        <extracomment>%1 is the network error message.</extracomment>
        <translation>Проверете връзката си, след което отворете отново този диалогов прозорец. %1</translation>
    </message>
    <message>
        <source>The language catalog returned an invalid response.</source>
        <translation>Езиковият каталог върна невалиден отговор.</translation>
    </message>
    <message>
        <source>Working...</source>
        <extracomment>Button text while a language downloads or is removed.</extracomment>
        <translation>Работи...</translation>
    </message>
    <message numerus="yes">
        <source>%n best-quality model(s) available</source>
        <extracomment>%n is the number of OCR language models listed.</extracomment>
        <translation>
            <numerusform>%n наличен модел с най-добро качество</numerusform>
            <numerusform>Налични %n модела с най-добро качество</numerusform>
        </translation>
    </message>
    <message>
        <source>Remove</source>
        <extracomment>Button on an installed OCR language. It deletes the language data.</extracomment>
        <translation>Премахнете</translation>
    </message>
    <message>
        <source>No OCR languages installed</source>
        <translation>Няма инсталирани OCR езици</translation>
    </message>
    <message>
        <source>Installed (%1)</source>
        <extracomment>Column heading. %1 is the number of installed OCR languages.</extracomment>
        <translation>Инсталирано (%1)</translation>
    </message>
    <message>
        <source>%1 total</source>
        <extracomment>%1 is the combined size of the installed OCR languages, such as &quot;48.2 MB&quot;.</extracomment>
        <translation>Общо %1</translation>
    </message>
    <message>
        <source>Add</source>
        <extracomment>Button on an available OCR language. It downloads and installs it.</extracomment>
        <translation>Добавете</translation>
    </message>
    <message>
        <source>No matching languages</source>
        <translation>Няма съответстващи езици</translation>
    </message>
    <message>
        <source>OCR Language Download</source>
        <translation>Изтегляне на OCR език</translation>
    </message>
    <message>
        <source>Could not download %1.

%2</source>
        <extracomment>%1 is an OCR language name, %2 the network error message.</extracomment>
        <translation>Неуспешно изтегляне на %1.

%2</translation>
    </message>
    <message>
        <source>Could not save %1 in the OCR language folder.</source>
        <extracomment>%1 is an OCR language name.</extracomment>
        <translation>Не може да се запише %1 в езиковата папка за OCR.</translation>
    </message>
    <message>
        <source>The downloaded model is not valid and was removed.

%1</source>
        <extracomment>%1 explains what is wrong with the file.</extracomment>
        <translation>Изтегленият модел не е валиден и беше премахнат.

%1</translation>
    </message>
    <message>
        <source>Remove OCR Language</source>
        <translation>Премахване на OCR език</translation>
    </message>
    <message>
        <source>Could not remove %1 from the OCR language folder.</source>
        <extracomment>%1 is an OCR language name.</extracomment>
        <translation>Не може да се премахне %1 от езиковата папка за OCR.</translation>
    </message>
</context>
<context>
    <name>mervin::MeasurePanel</name>
    <message>
        <source>Measure</source>
        <extracomment>Title of the measuring tool&apos;s panel. Use the same word as the Measure action, the verb or the word for measuring, not a noun for a step, a unit or a size.</extracomment>
        <translation>Измерете</translation>
    </message>
    <message>
        <source>Close measuring tool</source>
        <translation>Затворете измервателния инструмент</translation>
    </message>
    <message>
        <source>Measure - click the page to place points; toggle off to select text</source>
        <extracomment>Tooltip of the panel&apos;s button that turns measuring on. Start with the same word as the Measure action, the verb or the word for measuring.</extracomment>
        <translation>Измерване - щракнете върху страницата, за да поставите точки; изключете, за да изберете текст</translation>
    </message>
    <message>
        <source>Distance</source>
        <extracomment>Measurement type button (noun): a straight distance between two points.</extracomment>
        <translation>Разстояние</translation>
    </message>
    <message>
        <source>Measure a straight distance</source>
        <translation>Измерете право разстояние</translation>
    </message>
    <message>
        <source>Path</source>
        <extracomment>Measurement type button (noun): the length of a line with several segments.</extracomment>
        <translation>Дължина на полилиния</translation>
    </message>
    <message>
        <source>Measure a multi-segment length</source>
        <translation>Измерете дължина от няколко сегмента</translation>
    </message>
    <message>
        <source>Area</source>
        <extracomment>Measurement type button (noun): the area of a polygon.</extracomment>
        <translation>Площ</translation>
    </message>
    <message>
        <source>Measure a polygon area and perimeter</source>
        <translation>Измерете площта и периметъра на многоъгълник</translation>
    </message>
    <message>
        <source>Angle</source>
        <extracomment>Measurement type button (noun): the angle between two lines.</extracomment>
        <translation>Ъгъл</translation>
    </message>
    <message>
        <source>Measure an angle (three points)</source>
        <translation>Измерване на ъгъл (три точки)</translation>
    </message>
    <message>
        <source>Scale -</source>
        <extracomment>Placeholder until the drawing scale is known. The dash means &quot;no value yet&quot;.</extracomment>
        <translation>мащаб -</translation>
    </message>
    <message>
        <source>Unit</source>
        <extracomment>Label before a picker of length units (mm, cm, m, in, ft).</extracomment>
        <translation>единица</translation>
    </message>
    <message>
        <source>Decimals</source>
        <extracomment>Label before a number field: how many decimal places measured values show.</extracomment>
        <translation>Десетични знаци</translation>
    </message>
    <message>
        <source>Fewer decimals</source>
        <translation>По-малко десетични знаци</translation>
    </message>
    <message>
        <source>More decimals</source>
        <translation>Още десетични знаци</translation>
    </message>
    <message>
        <source>Line width</source>
        <extracomment>Label before a picker of stroke widths for measurement lines.</extracomment>
        <translation>Ширина на линията</translation>
    </message>
    <message>
        <source>Calibrate</source>
        <extracomment>Button (verb): set the drawing scale by drawing a line of known length.</extracomment>
        <translation>Калибриране</translation>
    </message>
    <message>
        <source>Set the scale by drawing a line of known length</source>
        <translation>Задайте мащаба, като начертаете линия с известна дължина</translation>
    </message>
    <message>
        <source>Set Scale</source>
        <extracomment>Button (verb): type the drawing scale as a ratio, such as 1 : 100.</extracomment>
        <translation>Задай мащаб</translation>
    </message>
    <message>
        <source>Set the scale manually by typing a ratio (1 : N)</source>
        <translation>Задайте мащаба ръчно, като въведете съотношение (1 : N)</translation>
    </message>
    <message>
        <source>Reset</source>
        <extracomment>Button (verb): drop the scale the user set and use the scale stored in the PDF.</extracomment>
        <translation>Възстанови мащаба</translation>
    </message>
    <message>
        <source>Discard the manual calibration and use the scale embedded in the PDF</source>
        <extracomment>Reset button tooltip. Removes the user&apos;s scale override from this page and restores its embedded PDF scale. Does not remove drawn measurements.</extracomment>
        <translation>Премахни ръчното калибриране и възстанови мащаба, вграден в PDF</translation>
    </message>
    <message>
        <source>Measurements</source>
        <extracomment>Heading above the list of measurements drawn on the document (noun, plural).</extracomment>
        <translation>Измервания</translation>
    </message>
    <message>
        <source>Clear all measurements</source>
        <translation>Изчистете всички измервания</translation>
    </message>
    <message>
        <source>Copy value</source>
        <translation>Копирай стойността</translation>
    </message>
    <message>
        <source>Delete measurement</source>
        <translation>Изтриване на измерването</translation>
    </message>
    <message>
        <source>Remove this measurement</source>
        <translation>Премахнете това измерване</translation>
    </message>
</context>
<context>
    <name>mervin::MergeDialog</name>
    <message>
        <source>Merge PDFs</source>
        <translation>Обединяване на PDF файлове</translation>
    </message>
    <message>
        <source>Files are merged top to bottom, in the order shown.</source>
        <translation>Файловете се обединяват отгоре надолу в показания ред.</translation>
    </message>
    <message>
        <source>File</source>
        <extracomment>Column caption over the file names.</extracomment>
        <translation>Файл</translation>
    </message>
    <message>
        <source>Pages, in order</source>
        <extracomment>Column caption over the page range fields.</extracomment>
        <translation>Страници, по ред</translation>
    </message>
    <message>
        <source>Add Files…</source>
        <translation>Добавяне на файлове...</translation>
    </message>
    <message>
        <source>Move Up</source>
        <translation>Преместване нагоре</translation>
    </message>
    <message>
        <source>Move Down</source>
        <translation>Преместване надолу</translation>
    </message>
    <message>
        <source>Duplicate</source>
        <extracomment>Button: adds a copy of the selected row below it.</extracomment>
        <translation>Дубликат</translation>
    </message>
    <message>
        <source>Remove</source>
        <extracomment>Button: removes the selected row from the list.</extracomment>
        <translation>Премахнете</translation>
    </message>
    <message>
        <source>The merged file will not be encrypted.</source>
        <extracomment>Shown below the merge result when the files include an encrypted one, with an open password or only permission restrictions, and more than one file is merged. The new PDF then has no password and no permission restrictions.</extracomment>
        <translation>Обединеният файл няма да бъде шифрован.</translation>
    </message>
    <message>
        <source>Save as:</source>
        <translation>Запиши като:</translation>
    </message>
    <message>
        <source>Choose where to write the merged PDF</source>
        <translation>Изберете къде да запишете обединения PDF файл</translation>
    </message>
    <message>
        <source>Browse…</source>
        <translation>Преглед...</translation>
    </message>
    <message>
        <source>Merge</source>
        <extracomment>Button: writes the merged PDF.</extracomment>
        <translation>Обединяване</translation>
    </message>
    <message>
        <source>The destination changed. Press Merge again to review it.</source>
        <extracomment>The output contents or target changed after this merge was approved. Nothing was replaced. Merge is the action button; pressing it checks the destination and asks before replacing an existing file.</extracomment>
        <translation>Целевият файл е променен. Натиснете „Обединяване“ отново, за да го проверите.</translation>
    </message>
    <message>
        <source>Could not check the destination. Press Merge to try again, or choose another name.</source>
        <extracomment>The destination could not be read to verify replacement consent. Merge remains available to retry. Save as can choose another output. Technical I/O details are shown separately.</extracomment>
        <translation>Целевият файл не може да бъде проверен. Натиснете „Обединяване“ за нов опит или изберете друго име.</translation>
    </message>
    <message>
        <source>The file contains no pages.</source>
        <translation>Файлът не съдържа страници.</translation>
    </message>
    <message>
        <source>This PDF is encrypted. Open it in Mervin, use Document &gt; Security to save an unlocked copy, then add that copy instead.</source>
        <extracomment>&quot;Document &gt; Security&quot; is the Security item in the Document menu. Use the same translated names as the menu.</extracomment>
        <translation>Този PDF е шифрован. Отворете го в Mervin и използвайте Документ &gt; Сигурност, за да запазите отключено копие. След това добавете копието.</translation>
    </message>
    <message>
        <source>Add PDFs to Merge</source>
        <translation>Добавяне на PDF към обединяване</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf)</source>
        <extracomment>File type filter. Keep &quot;(*.pdf)&quot; as it is.</extracomment>
        <translation>PDF документи (*.pdf)</translation>
    </message>
    <message>
        <source>%1 - %2</source>
        <extracomment>One line of the &quot;cannot be merged&quot; list: %1 is a file name, %2 why.</extracomment>
        <translation>%1 - %2</translation>
    </message>
    <message numerus="yes">
        <source>These files cannot be merged:

%1</source>
        <extracomment>%1 is a list of files, one per line. The number of files picks the singular or plural form.</extracomment>
        <translation>
            <numerusform>Тези файлове не могат да бъдат обединени:

%1</numerusform>
            <numerusform>Тези файлове не могат да бъдат обединени:

%1</numerusform>
        </translation>
    </message>
    <message>
        <source>Save Merged PDF</source>
        <translation>Запазване на обединения PDF</translation>
    </message>
    <message>
        <source>e.g. 1-3, 5</source>
        <extracomment>Placeholder of an empty page range field in the merge list.</extracomment>
        <translation>напр. 1-3, 5</translation>
    </message>
    <message>
        <source>Type a page range in the Pages column, for example 1-3, 5, 8-10 - or %1. Pages are taken in the order you type them.</source>
        <extracomment>%1 is the word for every page (PageRange &quot;All&quot;), which can be typed instead of a range. &quot;The Pages column&quot; is the column captioned &quot;Pages, in order&quot;.</extracomment>
        <translation>Въведете диапазон от страници в колоната Страници, например 1-3, 5, 8-10 - или %1. Страниците се вземат в реда, в който ги въвеждате.</translation>
    </message>
    <message>
        <source>Choose where to save the merged PDF.</source>
        <translation>Изберете къде да запазите обединения PDF файл.</translation>
    </message>
    <message>
        <source>That file is open in a tab. Choose another name.</source>
        <translation>Този файл е отворен в раздел. Изберете друго име.</translation>
    </message>
    <message>
        <source>The merged file cannot replace one of the files being merged. Choose a different name.</source>
        <translation>Обединеният файл не може да замени нито един от обединяваните файлове. Изберете друго име.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; already exists. Replace it?</source>
        <extracomment>%1 is a file path.</extracomment>
        <translation>„%1“ вече съществува. Да го замените?</translation>
    </message>
    <message>
        <source>The merge failed on &quot;%1&quot;.</source>
        <extracomment>%1 is the name of the file being merged when it failed.</extracomment>
        <translation>Обединяването не бе успешно на &quot;%1&quot;.</translation>
    </message>
    <message>
        <source>Could not write &quot;%1&quot;. If another program has it open, close it and press Merge again, or choose another name.</source>
        <extracomment>%1 is the merged file&apos;s name.</extracomment>
        <translation>Не може да се напише &quot;%1&quot;. Ако друга програма го е отворила, затворете го и натиснете Обединяване отново или изберете друго име.</translation>
    </message>
</context>
<context>
    <name>mervin::MergePlan</name>
    <message>
        <source>&quot;%1&quot; is still being read.</source>
        <extracomment>Shown after the row number in Merge&apos;s footer while a background probe reads the PDF. %1 is the file name, for example b.pdf. Merge stays disabled until reading finishes. This is a pending state, not a read failure.</extracomment>
        <translation>„%1“ все още се чете.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; is encrypted and cannot be merged.</source>
        <extracomment>%1 is a file name.</extracomment>
        <translation>„%1“ е шифрован и не може да бъде обединен.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; could not be read.</source>
        <extracomment>%1 is a file name.</extracomment>
        <translation>„%1“ не можа да бъде прочетен.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; has no pages.</source>
        <extracomment>%1 is a file name.</extracomment>
        <translation>„%1“ няма страници.</translation>
    </message>
    <message>
        <source>Enter pages like 1-3, 5, 8-10.</source>
        <translation>Въведете страници като 1-3, 5, 8-10.</translation>
    </message>
    <message>
        <source>Reading…</source>
        <extracomment>Count column of a Merge input while its background file probe runs. This means reading the PDF&apos;s information, not a page selection.</extracomment>
        <translation>Четене…</translation>
    </message>
    <message>
        <source>Locked</source>
        <extracomment>Count column of a merge row whose file is encrypted.</extracomment>
        <translation>Заключено</translation>
    </message>
    <message>
        <source>Unreadable</source>
        <extracomment>Count column of a merge row whose file could not be read.</extracomment>
        <translation>Нечетливо</translation>
    </message>
    <message>
        <source>%1 of %2</source>
        <extracomment>Count column of a merge row: %1 pages taken of the file&apos;s %2 pages.</extracomment>
        <translation>%1 от %2</translation>
    </message>
    <message>
        <source>Add PDFs to merge.</source>
        <translation>Добавете PDF файлове за обединяване.</translation>
    </message>
    <message>
        <source>This merge cannot run yet.</source>
        <translation>Това сливане все още не може да се изпълнява.</translation>
    </message>
    <message numerus="yes">
        <source>%n file(s)</source>
        <extracomment>Goes into %1 of &quot;Result: %n page(s) from %1, in the order shown.&quot;, so use the form that fits after &quot;from&quot;.</extracomment>
        <translation>
            <numerusform>%n файл</numerusform>
            <numerusform>%n файла</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Result: %n page(s) from %1, in the order shown.</source>
        <extracomment>%1 is the number of files, like &quot;4 files&quot;.</extracomment>
        <translation>
            <numerusform>Резултат: %n страница от %1, в показания ред.</numerusform>
            <numerusform>Резултат: %n страници от %1, в показания ред.</numerusform>
        </translation>
    </message>
    <message>
        <source>Row %1: %2</source>
        <extracomment>%1 is the row number, %2 the sentence describing the pending read or row problem.</extracomment>
        <translation>Ред %1: %2</translation>
    </message>
</context>
<context>
    <name>mervin::OcrPopup</name>
    <message>
        <source>OCR Selection</source>
        <translation>Избор на OCR</translation>
    </message>
    <message>
        <source>Language</source>
        <extracomment>Label for the OCR language picker.</extracomment>
        <translation>език</translation>
    </message>
    <message>
        <source>Manage Languages</source>
        <translation>Управление на езици</translation>
    </message>
    <message>
        <source>Recognised text will appear here; you can edit it before copying.</source>
        <translation>Тук ще се появи разпознат текст; можете да го редактирате преди копиране.</translation>
    </message>
    <message>
        <source>Preserve line breaks</source>
        <translation>Запазете прекъсванията на редовете</translation>
    </message>
    <message>
        <source>Auto-trim</source>
        <extracomment>Check box. Removes spaces and blank lines at the start and end of the text.</extracomment>
        <translation>Изрязване на крайните интервали</translation>
    </message>
    <message>
        <source>Copy</source>
        <extracomment>Button. Copies the recognized text to the clipboard.</extracomment>
        <translation>Копирай</translation>
    </message>
</context>
<context>
    <name>mervin::OcrService</name>
    <message>
        <source>Invalid OCR request.</source>
        <translation>Невалидна OCR заявка.</translation>
    </message>
</context>
<context>
    <name>mervin::OpenPdfDialog</name>
    <message>
        <source>Open PDF</source>
        <translation>Отворете PDF</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf)</source>
        <extracomment>File type filter in the Open dialog. Keep the pattern in parentheses as it is.</extracomment>
        <translation>PDF документи (*.pdf)</translation>
    </message>
    <message>
        <source>All files (*.*)</source>
        <extracomment>File type filter in the Open dialog. Keep the pattern in parentheses as it is.</extracomment>
        <translation>Всички файлове (*.*)</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf);;All files (*)</source>
        <extracomment>Two file type filters in the Open dialog, separated by &quot;;;&quot;. Keep the patterns in parentheses and the &quot;;;&quot; as they are.</extracomment>
        <translation>PDF документи (*.pdf);;Всички файлове (*)</translation>
    </message>
    <message>
        <source>File &amp;name or URL:</source>
        <extracomment>Label of the Open dialog&apos;s file name field, which also accepts a web address.</extracomment>
        <translation>&amp;Име на файл или URL:</translation>
    </message>
    <message>
        <source>&amp;Look in:</source>
        <extracomment>Label of the folder picker at the top of the Open dialog.</extracomment>
        <translation>&amp;Вижте в:</translation>
    </message>
    <message>
        <source>Files of &amp;type:</source>
        <extracomment>Label of the file type filter at the bottom of the Open dialog.</extracomment>
        <translation>&amp;Файлове от тип:</translation>
    </message>
</context>
<context>
    <name>mervin::OutlineSidebar</name>
    <message>
        <source>Page %1</source>
        <extracomment>Tooltip of an outline entry. %1 is the page number it leads to.</extracomment>
        <translation>Страница %1</translation>
    </message>
    <message>
        <source>(no outline)</source>
        <extracomment>Shown in the Outline sidebar when the document has no outline.</extracomment>
        <translation>(без съдържание)</translation>
    </message>
</context>
<context>
    <name>mervin::PageOps</name>
    <message>
        <source>Page does not exist.</source>
        <translation>Страницата не съществува.</translation>
    </message>
    <message>
        <source>Cannot delete every page.</source>
        <translation>Не може да се изтрият всички страници.</translation>
    </message>
    <message>
        <source>Rotation must be a multiple of 90 degrees.</source>
        <translation>Завъртането трябва да е кратно на 90 градуса.</translation>
    </message>
    <message numerus="yes">
        <source>It has %n page(s); page %1 does not exist.</source>
        <extracomment>Merge or extract error, shown below a line that names the file. %n is the file&apos;s page count, %1 the missing page number.</extracomment>
        <translation>
            <numerusform>Има %n страница; страница %1 не съществува.</numerusform>
            <numerusform>Има %n страници; страница %1 не съществува.</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::PdfPropertiesPopup</name>
    <message>
        <source>Properties</source>
        <extracomment>Title of a popup that lists the properties of a part or net in a schematic or circuit board drawing, read from the PDF (noun, plural).</extracomment>
        <translation>Свойства</translation>
    </message>
    <message>
        <source>Close</source>
        <extracomment>Tooltip of the button that closes this popup (verb).</extracomment>
        <translation>затвори</translation>
    </message>
    <message>
        <source>Copy property</source>
        <translation>Копирай свойството</translation>
    </message>
</context>
<context>
    <name>mervin::PrintPreviewWidget</name>
    <message>
        <source>Print preview</source>
        <translation>Преглед на печат</translation>
    </message>
    <message>
        <source>Could not render this page.</source>
        <translation>Тази страница не можа да се визуализира.</translation>
    </message>
    <message>
        <source>No page to preview.</source>
        <translation>Няма страница за преглед.</translation>
    </message>
    <message>
        <source>Loading preview…</source>
        <translation>Визуализацията се зарежда...</translation>
    </message>
</context>
<context>
    <name>mervin::QpdfService</name>
    <message>
        <source>A password is required to open this document.</source>
        <translation>Необходима е парола за отваряне на този документ.</translation>
    </message>
    <message>
        <source>None</source>
        <extracomment>Shown as the encryption algorithm when there is none.</extracomment>
        <translation>Няма</translation>
    </message>
    <message>
        <source>Unknown</source>
        <extracomment>Shown as the encryption algorithm when qpdf cannot identify it.</extracomment>
        <translation>неизвестен</translation>
    </message>
</context>
<context>
    <name>mervin::RecentFilesPanel</name>
    <message>
        <source>Stop</source>
        <extracomment>Button that stops the search inside the documents.</extracomment>
        <translation>Спрете</translation>
    </message>
    <message>
        <source>Open in new window</source>
        <translation>Отворете в нов прозорец</translation>
    </message>
    <message>
        <source>Clear missing files</source>
        <translation>Изчистете липсващите файлове</translation>
    </message>
    <message>
        <source>Searching…</source>
        <translation>Търсене...</translation>
    </message>
    <message>
        <source>Searching file %1 of %2</source>
        <extracomment>%1 is the file being read, %2 how many files the search reads.</extracomment>
        <translation>Търсене на файл %1 от %2</translation>
    </message>
    <message>
        <source>Stopped</source>
        <extracomment>The search inside the documents was stopped before it finished.</extracomment>
        <translation>Спряно</translation>
    </message>
    <message>
        <source>Favourites</source>
        <extracomment>Caption above the starred files in the Recent list.</extracomment>
        <translation>Любими</translation>
    </message>
    <message>
        <source>Recent</source>
        <extracomment>Caption above the files that are not starred in the Recent list.</extracomment>
        <translation>Скорошни</translation>
    </message>
    <message>
        <source>No recent files yet</source>
        <translation>Все още няма скорошни файлове</translation>
    </message>
    <message>
        <source>No file names contain &quot;%1&quot;</source>
        <translation>Никое име на файл не съдържа &quot;%1&quot;</translation>
    </message>
    <message>
        <source>No documents contain &quot;%1&quot;</source>
        <translation>Никой документ не съдържа &quot;%1&quot;</translation>
    </message>
    <message>
        <source>No file names or documents contain &quot;%1&quot;</source>
        <translation>Нито едно име на файл или документ не съдържа &quot;%1&quot;</translation>
    </message>
    <message>
        <source>%1 starred, %2 recent</source>
        <extracomment>Status bar summary of the Recent list. %1 is the number of starred files, %2 the number of other recent files. If the words must agree with the numbers, a form such as &quot;Starred: %1, recent: %2&quot; works.</extracomment>
        <translation>%1 със звезда, %2 скорошни</translation>
    </message>
    <message>
        <source>%1 · %2</source>
        <extracomment>Joins two details in a row of the Recent list, such as when the file was opened and the date (&quot;3 days ago · 03/10/2026&quot;), or its page count and size.</extracomment>
        <translation>%1 · %2</translation>
    </message>
    <message>
        <source>Today</source>
        <extracomment>When a recent file was last opened.</extracomment>
        <translation>Днес</translation>
    </message>
    <message>
        <source>Yesterday</source>
        <extracomment>When a recent file was last opened.</extracomment>
        <translation>Вчера</translation>
    </message>
    <message numerus="yes">
        <source>%n day(s) ago</source>
        <extracomment>When a recent file was last opened. Used from 2 days on.</extracomment>
        <translation>
            <numerusform>преди %n ден</numerusform>
            <numerusform>преди %n дни</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n page(s)</source>
        <translation>
            <numerusform>%n страница</numerusform>
            <numerusform>%n страници</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Stopped, %n file(s) found</source>
        <translation>
            <numerusform>Спрян, намерен е %n файл</numerusform>
            <numerusform>Спряно, намерени са %n файла</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) found</source>
        <translation>
            <numerusform>Намерен е %n файл</numerusform>
            <numerusform>Намерени са %n файла</numerusform>
        </translation>
    </message>
    <message>
        <source>%1
Match on page %2</source>
        <extracomment>Tooltip of a file found by the content search. %1 is the file&apos;s path, %2 the page with the first match.</extracomment>
        <translation>%1
Съвпадение на страница %2</translation>
    </message>
    <message>
        <source>%1
This file is no longer on disk.</source>
        <extracomment>Tooltip of a recent file that was moved or deleted. %1 is its path.</extracomment>
        <translation>%1
Този файл вече не е на диска.</translation>
    </message>
    <message numerus="yes">
        <source>%n result(s)</source>
        <translation>
            <numerusform>%n резултат</numerusform>
            <numerusform>%n резултата</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Your last %n opened document(s)</source>
        <extracomment>Status bar summary of the Recent list.</extracomment>
        <translation>
            <numerusform>Вашият последен %n отворен документ</numerusform>
            <numerusform>Вашите последни %n отворени документа</numerusform>
        </translation>
    </message>
    <message>
        <source>File not found</source>
        <translation>Файлът не е намерен</translation>
    </message>
    <message>
        <source>&quot;%1&quot; could not be found.</source>
        <extracomment>%1 is the file&apos;s path.</extracomment>
        <translation>„%1“ не можа да бъде намерен.</translation>
    </message>
    <message>
        <source>It may have been moved, renamed, or deleted.</source>
        <translation>Може да е бил преместен, преименуван или изтрит.</translation>
    </message>
    <message>
        <source>Locate</source>
        <extracomment>Button that opens a file dialog to find where the missing file is now.</extracomment>
        <translation>Намерете</translation>
    </message>
    <message>
        <source>Remove from history</source>
        <translation>Премахване от историята</translation>
    </message>
    <message>
        <source>Locate &quot;%1&quot;</source>
        <extracomment>Title of the file dialog. %1 is the missing file&apos;s name.</extracomment>
        <translation>Намерете &quot;%1&quot;</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf);;All files (*)</source>
        <extracomment>File type filter. Keep the patterns in parentheses and the &quot;;;&quot;.</extracomment>
        <translation>PDF документи (*.pdf);;Всички файлове (*)</translation>
    </message>
</context>
<context>
    <name>mervin::RecentSearchField</name>
    <message>
        <source>Clear</source>
        <extracomment>Tooltip of the button that empties the search field.</extracomment>
        <translation>Изчисти</translation>
    </message>
    <message>
        <source>Names</source>
        <extracomment>Search scope toggle that searches the file names.</extracomment>
        <translation>имена</translation>
    </message>
    <message>
        <source>Search file names</source>
        <translation>Търсете имена на файлове</translation>
    </message>
    <message>
        <source>Contents</source>
        <extracomment>Search scope toggle that searches the text inside the documents.</extracomment>
        <translation>Съдържание</translation>
    </message>
    <message>
        <source>Search inside documents</source>
        <translation>Търсене в документи</translation>
    </message>
    <message>
        <source>All</source>
        <extracomment>Search scope toggle that searches file names, then inside the documents.</extracomment>
        <translation>Всички</translation>
    </message>
    <message>
        <source>Search file names, then inside documents</source>
        <translation>Търсене в имена на файлове, след това в документи</translation>
    </message>
    <message>
        <source>Search names and contents</source>
        <translation>Търсете имена и съдържание</translation>
    </message>
</context>
<context>
    <name>mervin::RenderEngine</name>
    <message>
        <source>Could not allocate a document context.</source>
        <translation>Не може да се разпредели контекст на документ.</translation>
    </message>
    <message>
        <source>Could not read the document.</source>
        <extracomment>Shown when the complete source file cannot be read. A partially read file is not opened, even if the PDF reader could repair it.</extracomment>
        <translation>Документът не може да бъде прочетен.</translation>
    </message>
    <message>
        <source>The file was not found.</source>
        <extracomment>Reason in a failed document&apos;s placeholder. The reader could not find the source file or one of the folders leading to it.</extracomment>
        <translation>Файлът не е намерен.</translation>
    </message>
    <message>
        <source>The file is damaged.</source>
        <extracomment>Reason in a failed document&apos;s placeholder. Its data has an unrecoverable format error, rather than a drive or password error.</extracomment>
        <translation>Файлът е повреден.</translation>
    </message>
    <message>
        <source>This document is password-protected.</source>
        <translation>Този документ е защитен с парола.</translation>
    </message>
    <message>
        <source>The password was not accepted.</source>
        <extracomment>Reason in a failed document&apos;s placeholder after its supplied open password was rejected. Retry can ask for another password.</extracomment>
        <translation>Паролата не е приета.</translation>
    </message>
</context>
<context>
    <name>mervin::RowList</name>
    <message>
        <source>Count</source>
        <extracomment>Column caption: how many pages the row contributes.</extracomment>
        <translation>Брой страници</translation>
    </message>
    <message>
        <source>Output</source>
        <extracomment>Column caption: where the row&apos;s pages land in the new file, like &quot;13-16&quot;.</extracomment>
        <translation>Изходни страници</translation>
    </message>
    <message>
        <source>Drag to reorder</source>
        <translation>Плъзнете, за да пренаредите</translation>
    </message>
    <message>
        <source>Remove this row</source>
        <translation>Премахнете този ред</translation>
    </message>
</context>
<context>
    <name>mervin::SaveCheck</name>
    <message>
        <source>Checking the folder…</source>
        <extracomment>Shown under Save as while the app checks the folder the file would be saved in. On a network drive this can take a while; the button that saves stays disabled until the check is done.</extracomment>
        <translation>Проверка на папката...</translation>
    </message>
</context>
<context>
    <name>mervin::SecurityDialog</name>
    <message>
        <source>Document Security</source>
        <translation>Сигурност на документите</translation>
    </message>
    <message>
        <source>Current security</source>
        <translation>Текуща сигурност</translation>
    </message>
    <message>
        <source>Encrypt / set password</source>
        <translation>Шифроване / задаване на парола</translation>
    </message>
    <message>
        <source>leave empty for no open password</source>
        <extracomment>Placeholder in the open-password field. An empty value means the saved PDF can be opened without entering a password.</extracomment>
        <translation>оставете празно, за да отворите без парола</translation>
    </message>
    <message>
        <source>defaults to the open password</source>
        <extracomment>Placeholder in the owner-password field. If empty, use the open password to protect the PDF permission settings. The user can enter a different one.</extracomment>
        <translation>ако е празно, използва се паролата за отваряне</translation>
    </message>
    <message>
        <source>AES-256 (recommended)</source>
        <extracomment>Encryption algorithm choice. Keep the algorithm name as it is.</extracomment>
        <translation>AES-256 (препоръчително)</translation>
    </message>
    <message>
        <source>AES-128</source>
        <extracomment>Encryption algorithm choice. Keep the algorithm name as it is.</extracomment>
        <translation>AES-128</translation>
    </message>
    <message>
        <source>RC4-128 (weak)</source>
        <extracomment>Encryption algorithm choice. Keep the algorithm name as it is.</extracomment>
        <translation>RC4-128 (слаб)</translation>
    </message>
    <message>
        <source>Allow printing</source>
        <translation>Разрешаване на печат</translation>
    </message>
    <message>
        <source>Allow copying text</source>
        <translation>Разрешаване на копиране на текст</translation>
    </message>
    <message>
        <source>Allow modifying</source>
        <translation>Разрешаване на модифициране</translation>
    </message>
    <message>
        <source>Allow annotating</source>
        <translation>Разрешаване на анотиране</translation>
    </message>
    <message>
        <source>Open password</source>
        <extracomment>The password needed to open the document.</extracomment>
        <translation>Парола за отваряне</translation>
    </message>
    <message>
        <source>Owner password</source>
        <extracomment>The password that guards the permission settings below.</extracomment>
        <translation>Парола за разрешенията</translation>
    </message>
    <message>
        <source>Algorithm</source>
        <extracomment>Label for the encryption algorithm picker.</extracomment>
        <translation>Алгоритъм</translation>
    </message>
    <message>
        <source>Encrypt and Save As</source>
        <translation>Шифроване и запазване като</translation>
    </message>
    <message>
        <source>Remove encryption</source>
        <translation>Премахнете криптирането</translation>
    </message>
    <message>
        <source>Remove Password (Save Decrypted Copy)</source>
        <translation>Премахване на парола (запазване на декриптирано копие)</translation>
    </message>
    <message>
        <source>Strip Owner Restrictions</source>
        <translation>Премахнете ограниченията за разрешения за документи</translation>
    </message>
    <message>
        <source>&lt;b&gt;Could not read security information.&lt;/b&gt;&lt;br&gt;%1</source>
        <extracomment>%1 is the reason.</extracomment>
        <translation>&lt;b&gt;Не може да се прочете информацията за сигурност.&lt;/b&gt;&lt;br&gt;%1</translation>
    </message>
    <message>
        <source>A password is required.</source>
        <translation>Изисква се парола.</translation>
    </message>
    <message>
        <source>Allowed</source>
        <extracomment>Permission state, fills %3 to %6 of the encryption summary.</extracomment>
        <translation>Разрешено</translation>
    </message>
    <message>
        <source>Not allowed</source>
        <extracomment>Permission state, fills %3 to %6 of the encryption summary.</extracomment>
        <translation>Не е позволено</translation>
    </message>
    <message>
        <source>This document is &lt;b&gt;not encrypted&lt;/b&gt;.</source>
        <translation>Този документ &lt;b&gt;не е шифрован&lt;/b&gt;.</translation>
    </message>
    <message>
        <source>&lt;b&gt;Encrypted&lt;/b&gt; · %1 (%2-bit)&lt;br&gt;Printing: %3 · Copying: %4 · Modifying: %5 · Annotating: %6&lt;br&gt;&lt;i&gt;Note: Mervin never enforces these permission flags.&lt;/i&gt;</source>
        <extracomment>%1 is the encryption algorithm, such as AES-256. %2 is the key length in bits. %3 to %6 are Allowed or Not allowed.</extracomment>
        <translation>&lt;b&gt;Криптиран&lt;/b&gt; · %1 (%2-битов)&lt;br&gt;Печат: %3 · Копиране: %4 · Модифициране: %5 · Анотиране: %6&lt;br&gt;&lt;i&gt;Забележка: Mervin никога не налага тези ограничения на разрешенията.&lt;/i&gt;</translation>
    </message>
    <message>
        <source>Save As</source>
        <translation>Запиши като</translation>
    </message>
    <message>
        <source>PDF documents (*.pdf)</source>
        <extracomment>File type filter. Keep &quot;(*.pdf)&quot; as it is.</extracomment>
        <translation>PDF документи (*.pdf)</translation>
    </message>
    <message>
        <source>This writes a copy with all owner-password restrictions removed (printing, copying, and editing become permitted).

Continue?</source>
        <translation>Това записва копие с премахнати всички ограничения на паролата на собственика (печатът, копирането и редактирането стават разрешени).

Продължаване?</translation>
    </message>
    <message>
        <source>Operation failed</source>
        <translation>Операцията е неуспешна</translation>
    </message>
    <message>
        <source>The correct password is required.</source>
        <translation>Изисква се правилната парола.</translation>
    </message>
    <message>
        <source>&quot;%1&quot; already exists. Replace it?</source>
        <extracomment>%1 is the destination filename. The background check captured its contents before this question. Yes approves replacing that version; nothing has been replaced yet. A changed file will require another decision.</extracomment>
        <translation>„%1“ вече съществува. Да се замени ли?</translation>
    </message>
    <message>
        <source>The destination changed. Try again to review it.</source>
        <extracomment>The output contents or target changed after this Security action was approved. Nothing was replaced. Security reopens with the same choices. Try again means choosing Encrypt, Remove Password or Strip Owner Restrictions again to check the destination and confirm any replacement of an existing file.</extracomment>
        <translation>Целевият файл е променен. Опитайте отново, за да го проверите.</translation>
    </message>
    <message>
        <source>Could not check the destination. Try again or choose another name.</source>
        <extracomment>The destination could not be read to verify replacement consent. Nothing was replaced. Security reopens with the same choices so the user can retry the action or choose another output filename in its Save As picker.</extracomment>
        <translation>Целевият файл не може да бъде проверен. Опитайте отново или изберете друго име.</translation>
    </message>
    <message>
        <source>The operation failed.

%1</source>
        <extracomment>%1 is the reason the security copy could not be written.</extracomment>
        <translation>Операцията се провали.

%1</translation>
    </message>
</context>
<context>
    <name>mervin::TabPage</name>
    <message>
        <source>Could not create the document recovery folder.</source>
        <translation>Не можа да се създаде папка за възстановяване на документа.</translation>
    </message>
    <message>
        <source>Opening %1 from %2. The drive is not answering.</source>
        <extracomment>Shown in place of the page when the drive has not answered for a few seconds while the document loads. Below it is the Stop waiting button. %1 is the file name (plan.pdf), %2 the folder it is read from (/nas/docs, or a network share folder on Windows).</extracomment>
        <translation>Отваряне на %1 от %2. Устройството не отговаря.</translation>
    </message>
    <message>
        <source>Saving to %1. The drive is not answering.</source>
        <extracomment>Shown in a bar over the document when the drive has not answered for a few seconds while a tool such as Rotate Pages or Split writes its files, or reads a file in the folder it writes to. On its right is the Stop waiting button. %1 is the folder (/nas/docs, or a network share folder on Windows).</extracomment>
        <translation>Запазване в %1. Устройството не отговаря.</translation>
    </message>
    <message>
        <source>Reading %1 from %2. The drive is not answering.</source>
        <extracomment>Shown in a bar over the document when the drive has not answered for a few seconds while a tool such as Rotate Pages reads a file. On its right is the Stop waiting button. %1 is the file name (plan.pdf), %2 the folder it is read from (/nas/docs, or a network share folder on Windows).</extracomment>
        <translation>Четене на %1 от %2. Устройството не отговаря.</translation>
    </message>
    <message>
        <source>Retry</source>
        <extracomment>Button that tries to load the document again.</extracomment>
        <translation>Опитайте отново</translation>
    </message>
    <message>
        <source>Stop waiting</source>
        <extracomment>Button beside a notice that the drive is not answering. Under the notice of a document that loads, it gives up the load and leaves the document unloaded. In the bar over a document that a tool works on, it gives up the tool&apos;s work. A stopped job that still uses the document keeps it unavailable until it releases it.</extracomment>
        <translation>Спрете да чакате</translation>
    </message>
    <message>
        <source>This file is being written. Try again when the operation finishes.</source>
        <extracomment>Reason when opening a page-tool output is refused until the tool finishes writing it. This is an active write reservation, not a password or permission.</extracomment>
        <translation>В този файл се записва. Опитайте отново, когато операцията приключи.</translation>
    </message>
    <message>
        <source>Document is unloaded.</source>
        <translation>Документът е премахнат от паметта.</translation>
    </message>
    <message>
        <source>The document is still loading.</source>
        <translation>Документът все още се зарежда.</translation>
    </message>
    <message>
        <source>Could not protect the document recovery copy.</source>
        <translation>Копието за възстановяване на документа не можа да бъде защитено.</translation>
    </message>
    <message>
        <source>Could not load this document.</source>
        <translation>Този документ не можа да се зареди.</translation>
    </message>
    <message>
        <source>Loading</source>
        <extracomment>Shown in place of the page while the document loads.</extracomment>
        <translation>Зарежда се</translation>
    </message>
    <message>
        <source>Stopped waiting. The document stays locked until the drive answers.</source>
        <extracomment>A stopped job still holds the shown document while a file call finishes. Locked means unavailable for viewing or editing, not password protection or file permissions. The tab becomes usable when the job releases it.</extracomment>
        <translation>Чакането е спряно. Документът остава блокиран, докато устройството не отговори.</translation>
    </message>
    <message>
        <source>Printing to %1. The printer is not answering.</source>
        <extracomment>A print job has waited for the printer system without progress. %1 is the printer name, such as Office Laser. Stop waiting leaves the blocked printer call on a worker and prevents further pages from being submitted.</extracomment>
        <translation>Печат на %1. Принтерът не отговаря.</translation>
    </message>
</context>
<context>
    <name>mervin::TessdataFile</name>
    <message>
        <source>%1 is damaged or incomplete (%2). Replace it with a fresh copy of the language data.</source>
        <extracomment>%1 is a language data file name such as swe.traineddata. %2 is a technical detail, one of the lower-case messages in this context.</extracomment>
        <translation>%1 е повреден или непълен (%2). Заменете го с ново копие на езиковите данни.</translation>
    </message>
    <message>
        <source>component %1 does not start with a unichar count</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number. A unichar is one character entry in Tesseract&apos;s character list.</extracomment>
        <translation>компонент %1 не започва с unichar брой</translation>
    </message>
    <message>
        <source>component %1 declares %2 unichars but holds %3</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number, %2 and %3 are counts.</extracomment>
        <translation>компонент %1 декларира %2 unichars, но съдържа %3</translation>
    </message>
    <message>
        <source>component %1 has an empty unichar entry</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number.</extracomment>
        <translation>компонент %1 има празен запис unichar</translation>
    </message>
    <message>
        <source>component %1 lists the unichar &quot;%2&quot; twice</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number, %2 the repeated character.</extracomment>
        <translation>компонент %1 изброява unichar &quot;%2&quot; два пъти</translation>
    </message>
    <message>
        <source>Cannot read %1: %2</source>
        <extracomment>%1 is a language data file name, %2 the system&apos;s reason.</extracomment>
        <translation>Не може да се прочете %1: %2</translation>
    </message>
    <message>
        <source>file is too small to hold a header</source>
        <extracomment>Detail for the damaged language data message.</extracomment>
        <translation>файлът е твърде малък, за да побере заглавка</translation>
    </message>
    <message>
        <source>header is truncated</source>
        <extracomment>Detail for the damaged language data message.</extracomment>
        <translation>заглавката е съкратена</translation>
    </message>
    <message>
        <source>implausible component count %1 - this is probably not Tesseract language data</source>
        <extracomment>Detail for the damaged language data message. %1 is a number.</extracomment>
        <translation>неправдоподобен брой компоненти %1 - това вероятно не са данни на езика Tesseract</translation>
    </message>
    <message>
        <source>offset table needs %1 bytes but the file is only %2</source>
        <extracomment>Detail for the damaged language data message. %1 and %2 are byte counts.</extracomment>
        <translation>офсетната таблица се нуждае от %1 байта, но файлът е само %2</translation>
    </message>
    <message>
        <source>offset table is truncated</source>
        <extracomment>Detail for the damaged language data message.</extracomment>
        <translation>офсетната таблица е съкратена</translation>
    </message>
    <message>
        <source>component %1 starts at %2, outside the %3-byte file</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number, %2 a byte offset, %3 the file size in bytes.</extracomment>
        <translation>компонент %1 започва от %2, извън %3-байтовия файл</translation>
    </message>
    <message>
        <source>component %1 starts at %2, before the previous component</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number, %2 a byte offset.</extracomment>
        <translation>компонент %1 започва от %2, преди предишния компонент</translation>
    </message>
    <message>
        <source>component %1 has a non-positive length %2</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number, %2 a length in bytes.</extracomment>
        <translation>компонент %1 има неположителна дължина %2</translation>
    </message>
    <message>
        <source>cannot seek to component %1</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number.</extracomment>
        <translation>не може да търси към компонент %1</translation>
    </message>
    <message>
        <source>component %1 is truncated</source>
        <extracomment>Detail for the damaged language data message. %1 is a component number.</extracomment>
        <translation>компонент %1 е съкратен</translation>
    </message>
</context>
<context>
    <name>mervin::TessdataManager</name>
    <message>
        <source>Chinese (Simplified)</source>
        <extracomment>OCR language model name.</extracomment>
        <translation>китайски (опростен)</translation>
    </message>
    <message>
        <source>Chinese (Traditional)</source>
        <extracomment>OCR language model name.</extracomment>
        <translation>китайски (традиционен)</translation>
    </message>
    <message>
        <source>German Fraktur</source>
        <extracomment>OCR language model name. Fraktur is the German blackletter typeface.</extracomment>
        <translation>Немски Fraktur</translation>
    </message>
    <message>
        <source>Math / equation detection</source>
        <extracomment>OCR model name. The model finds math formulas instead of reading a language.</extracomment>
        <translation>Откриване на математика / уравнение</translation>
    </message>
    <message>
        <source>Orientation and script detection</source>
        <extracomment>OCR model name. The model finds page orientation and writing system instead of reading a language.</extracomment>
        <translation>Откриване на ориентация на страницата и система за писане</translation>
    </message>
    <message>
        <source>Afrikaans</source>
        <extracomment>OCR language name.</extracomment>
        <translation>африкаанс</translation>
    </message>
    <message>
        <source>Amharic</source>
        <extracomment>OCR language name.</extracomment>
        <translation>амхарски</translation>
    </message>
    <message>
        <source>Arabic</source>
        <extracomment>OCR language name.</extracomment>
        <translation>арабски</translation>
    </message>
    <message>
        <source>Assamese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>асамски</translation>
    </message>
    <message>
        <source>Azerbaijani</source>
        <extracomment>OCR language name.</extracomment>
        <translation>азербайджански</translation>
    </message>
    <message>
        <source>Belarusian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>белоруски</translation>
    </message>
    <message>
        <source>Bangla</source>
        <extracomment>OCR language name.</extracomment>
        <translation>бангла</translation>
    </message>
    <message>
        <source>Tibetan</source>
        <extracomment>OCR language name.</extracomment>
        <translation>тибетски</translation>
    </message>
    <message>
        <source>Bosnian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>босненски</translation>
    </message>
    <message>
        <source>Breton</source>
        <extracomment>OCR language name.</extracomment>
        <translation>бретонски</translation>
    </message>
    <message>
        <source>Bulgarian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>български</translation>
    </message>
    <message>
        <source>Catalan</source>
        <extracomment>OCR language name.</extracomment>
        <translation>каталонски</translation>
    </message>
    <message>
        <source>Cebuano</source>
        <extracomment>OCR language name.</extracomment>
        <translation>себуано</translation>
    </message>
    <message>
        <source>Czech</source>
        <extracomment>OCR language name.</extracomment>
        <translation>чешки</translation>
    </message>
    <message>
        <source>Cherokee</source>
        <extracomment>OCR language name.</extracomment>
        <translation>чероки</translation>
    </message>
    <message>
        <source>Corsican</source>
        <extracomment>OCR language name.</extracomment>
        <translation>корсикански</translation>
    </message>
    <message>
        <source>Welsh</source>
        <extracomment>OCR language name.</extracomment>
        <translation>уелски</translation>
    </message>
    <message>
        <source>Danish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>датски</translation>
    </message>
    <message>
        <source>German</source>
        <extracomment>OCR language name.</extracomment>
        <translation>немски</translation>
    </message>
    <message>
        <source>Divehi</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Дивехи</translation>
    </message>
    <message>
        <source>Dzongkha</source>
        <extracomment>OCR language name.</extracomment>
        <translation>дзонгкха</translation>
    </message>
    <message>
        <source>Greek</source>
        <extracomment>OCR language name.</extracomment>
        <translation>гръцки</translation>
    </message>
    <message>
        <source>English</source>
        <extracomment>OCR language name.</extracomment>
        <translation>английски</translation>
    </message>
    <message>
        <source>Esperanto</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Есперанто</translation>
    </message>
    <message>
        <source>Estonian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>естонски</translation>
    </message>
    <message>
        <source>Basque</source>
        <extracomment>OCR language name.</extracomment>
        <translation>баски</translation>
    </message>
    <message>
        <source>Faroese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>фарьорски</translation>
    </message>
    <message>
        <source>Persian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>персийски</translation>
    </message>
    <message>
        <source>Filipino</source>
        <extracomment>OCR language name.</extracomment>
        <translation>филипински</translation>
    </message>
    <message>
        <source>Finnish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>финландски</translation>
    </message>
    <message>
        <source>French</source>
        <extracomment>OCR language name.</extracomment>
        <translation>френски</translation>
    </message>
    <message>
        <source>Western Frisian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>западен фризийски</translation>
    </message>
    <message>
        <source>Scottish Gaelic</source>
        <extracomment>OCR language name.</extracomment>
        <translation>шотландски келтски</translation>
    </message>
    <message>
        <source>Irish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>ирландски</translation>
    </message>
    <message>
        <source>Galician</source>
        <extracomment>OCR language name.</extracomment>
        <translation>галисийски</translation>
    </message>
    <message>
        <source>Ancient Greek</source>
        <extracomment>OCR language name.</extracomment>
        <translation>старогръцки</translation>
    </message>
    <message>
        <source>Gujarati</source>
        <extracomment>OCR language name.</extracomment>
        <translation>гуджарати</translation>
    </message>
    <message>
        <source>Haitian Creole</source>
        <extracomment>OCR language name.</extracomment>
        <translation>хаитянски креолски</translation>
    </message>
    <message>
        <source>Hebrew</source>
        <extracomment>OCR language name.</extracomment>
        <translation>иврит</translation>
    </message>
    <message>
        <source>Hindi</source>
        <extracomment>OCR language name.</extracomment>
        <translation>хинди</translation>
    </message>
    <message>
        <source>Croatian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>хърватски</translation>
    </message>
    <message>
        <source>Hungarian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>унгарски</translation>
    </message>
    <message>
        <source>Armenian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>арменски</translation>
    </message>
    <message>
        <source>Inuktitut</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Инуктитут</translation>
    </message>
    <message>
        <source>Indonesian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>индонезийски</translation>
    </message>
    <message>
        <source>Icelandic</source>
        <extracomment>OCR language name.</extracomment>
        <translation>исландски</translation>
    </message>
    <message>
        <source>Italian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>италиански</translation>
    </message>
    <message>
        <source>Javanese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>явански</translation>
    </message>
    <message>
        <source>Japanese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>японски</translation>
    </message>
    <message>
        <source>Kannada</source>
        <extracomment>OCR language name.</extracomment>
        <translation>каннада</translation>
    </message>
    <message>
        <source>Georgian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>грузински</translation>
    </message>
    <message>
        <source>Kazakh</source>
        <extracomment>OCR language name.</extracomment>
        <translation>казахски</translation>
    </message>
    <message>
        <source>Khmer</source>
        <extracomment>OCR language name.</extracomment>
        <translation>кхмерски</translation>
    </message>
    <message>
        <source>Kyrgyz</source>
        <extracomment>OCR language name.</extracomment>
        <translation>киргизки</translation>
    </message>
    <message>
        <source>Korean</source>
        <extracomment>OCR language name.</extracomment>
        <translation>корейски</translation>
    </message>
    <message>
        <source>Lao</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Лаос</translation>
    </message>
    <message>
        <source>Latin</source>
        <extracomment>OCR language name.</extracomment>
        <translation>латински</translation>
    </message>
    <message>
        <source>Latvian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>латвийски</translation>
    </message>
    <message>
        <source>Lithuanian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>литовски</translation>
    </message>
    <message>
        <source>Luxembourgish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>люксембургски</translation>
    </message>
    <message>
        <source>Malayalam</source>
        <extracomment>OCR language name.</extracomment>
        <translation>малаялам</translation>
    </message>
    <message>
        <source>Marathi</source>
        <extracomment>OCR language name.</extracomment>
        <translation>маратхи</translation>
    </message>
    <message>
        <source>Macedonian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>македонски</translation>
    </message>
    <message>
        <source>Maltese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>малтийски</translation>
    </message>
    <message>
        <source>Mongolian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>монголски</translation>
    </message>
    <message>
        <source>Māori</source>
        <extracomment>OCR language name.</extracomment>
        <translation>маори</translation>
    </message>
    <message>
        <source>Malay</source>
        <extracomment>OCR language name.</extracomment>
        <translation>малайски</translation>
    </message>
    <message>
        <source>Burmese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>бирманска</translation>
    </message>
    <message>
        <source>Nepali</source>
        <extracomment>OCR language name.</extracomment>
        <translation>непалски</translation>
    </message>
    <message>
        <source>Dutch</source>
        <extracomment>OCR language name.</extracomment>
        <translation>холандски</translation>
    </message>
    <message>
        <source>Occitan</source>
        <extracomment>OCR language name.</extracomment>
        <translation>окситански</translation>
    </message>
    <message>
        <source>Odia</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Одия</translation>
    </message>
    <message>
        <source>Punjabi</source>
        <extracomment>OCR language name.</extracomment>
        <translation>пенджабски</translation>
    </message>
    <message>
        <source>Polish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>полски</translation>
    </message>
    <message>
        <source>Portuguese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>португалски</translation>
    </message>
    <message>
        <source>Pashto</source>
        <extracomment>OCR language name.</extracomment>
        <translation>пущу</translation>
    </message>
    <message>
        <source>Quechua</source>
        <extracomment>OCR language name.</extracomment>
        <translation>кечуа</translation>
    </message>
    <message>
        <source>Romanian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>румънски</translation>
    </message>
    <message>
        <source>Russian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>руски</translation>
    </message>
    <message>
        <source>Sanskrit</source>
        <extracomment>OCR language name.</extracomment>
        <translation>санскрит</translation>
    </message>
    <message>
        <source>Sinhala</source>
        <extracomment>OCR language name.</extracomment>
        <translation>сингалски</translation>
    </message>
    <message>
        <source>Slovak</source>
        <extracomment>OCR language name.</extracomment>
        <translation>словашки</translation>
    </message>
    <message>
        <source>Slovenian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>словенски</translation>
    </message>
    <message>
        <source>Sindhi</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Синдхи</translation>
    </message>
    <message>
        <source>Spanish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>испански</translation>
    </message>
    <message>
        <source>Albanian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>албански</translation>
    </message>
    <message>
        <source>Serbian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>сръбски</translation>
    </message>
    <message>
        <source>Sundanese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>сундански</translation>
    </message>
    <message>
        <source>Swahili</source>
        <extracomment>OCR language name.</extracomment>
        <translation>суахили</translation>
    </message>
    <message>
        <source>Swedish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>шведски</translation>
    </message>
    <message>
        <source>Syriac</source>
        <extracomment>OCR language name.</extracomment>
        <translation>сирийски</translation>
    </message>
    <message>
        <source>Tamil</source>
        <extracomment>OCR language name.</extracomment>
        <translation>тамилски</translation>
    </message>
    <message>
        <source>Tatar</source>
        <extracomment>OCR language name.</extracomment>
        <translation>татарски</translation>
    </message>
    <message>
        <source>Telugu</source>
        <extracomment>OCR language name.</extracomment>
        <translation>телугу</translation>
    </message>
    <message>
        <source>Tajik</source>
        <extracomment>OCR language name.</extracomment>
        <translation>таджикски</translation>
    </message>
    <message>
        <source>Thai</source>
        <extracomment>OCR language name.</extracomment>
        <translation>тайландски</translation>
    </message>
    <message>
        <source>Tigrinya</source>
        <extracomment>OCR language name.</extracomment>
        <translation>Тигриня</translation>
    </message>
    <message>
        <source>Tongan</source>
        <extracomment>OCR language name.</extracomment>
        <translation>тонгански</translation>
    </message>
    <message>
        <source>Turkish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>турски</translation>
    </message>
    <message>
        <source>Uyghur</source>
        <extracomment>OCR language name.</extracomment>
        <translation>уйгурски</translation>
    </message>
    <message>
        <source>Ukrainian</source>
        <extracomment>OCR language name.</extracomment>
        <translation>украински</translation>
    </message>
    <message>
        <source>Urdu</source>
        <extracomment>OCR language name.</extracomment>
        <translation>урду</translation>
    </message>
    <message>
        <source>Uzbek</source>
        <extracomment>OCR language name.</extracomment>
        <translation>узбекски</translation>
    </message>
    <message>
        <source>Vietnamese</source>
        <extracomment>OCR language name.</extracomment>
        <translation>виетнамски</translation>
    </message>
    <message>
        <source>Yiddish</source>
        <extracomment>OCR language name.</extracomment>
        <translation>идиш</translation>
    </message>
    <message>
        <source>Yoruba</source>
        <extracomment>OCR language name.</extracomment>
        <translation>йоруба</translation>
    </message>
</context>
<context>
    <name>mervin::UiThemePicker</name>
    <message>
        <source>UI theme</source>
        <translation>Тема на потребителския интерфейс</translation>
    </message>
    <message>
        <source>Dark</source>
        <extracomment>UI theme name: dark windows.</extracomment>
        <translation>Тъмно</translation>
    </message>
    <message>
        <source>Light</source>
        <extracomment>UI theme name: light windows.</extracomment>
        <translation>Светла</translation>
    </message>
    <message>
        <source>Follow system</source>
        <extracomment>UI theme choice: dark or light as the operating system is set.</extracomment>
        <translation>Следвайте системата</translation>
    </message>
    <message>
        <source>Dark window, toolbars and dialogs</source>
        <translation>Тъмен прозорец, ленти с инструменти и диалогови прозорци</translation>
    </message>
    <message>
        <source>Light window, toolbars and dialogs</source>
        <translation>Светъл прозорец, ленти с инструменти и диалогови прозорци</translation>
    </message>
    <message>
        <source>Switches between light and dark with the operating system</source>
        <translation>Превключва между светло и тъмно с операционната система</translation>
    </message>
</context>
<context>
    <name>mervin::Updater</name>
    <message>
        <source>Update</source>
        <extracomment>Window title (noun): the software update.</extracomment>
        <translation>Актуализация</translation>
    </message>
    <message>
        <source>Couldn&apos;t check for updates right now.

%1</source>
        <extracomment>%1 says why, such as a network error message.</extracomment>
        <translation>Не можах да проверя за актуализации в момента.

%1</translation>
    </message>
    <message>
        <source>The update server&apos;s answer couldn&apos;t be read.</source>
        <extracomment>Shown below &quot;Couldn&apos;t check for updates right now.&quot; when the update server&apos;s answer was not release information, such as a web page from a network filter.</extracomment>
        <translation>Отговорът на сървъра за актуализации не можа да бъде прочетен.</translation>
    </message>
    <message>
        <source>Check for Updates</source>
        <translation>Проверете за актуализации</translation>
    </message>
    <message>
        <source>You&apos;re up to date.

Mervin PDF %1 is the latest version.</source>
        <extracomment>%1 is the version number of this copy.</extracomment>
        <translation>Вие сте в крак с времето.

Mervin PDF %1 е най-новата версия.</translation>
    </message>
    <message>
        <source>Update Available</source>
        <translation>Налична актуализация</translation>
    </message>
    <message>
        <source>Mervin PDF %1 is available.</source>
        <extracomment>%1 is the version number of the new release.</extracomment>
        <translation>Mervin PDF %1 е наличен.</translation>
    </message>
    <message>
        <source>You have %1.</source>
        <extracomment>%1 is the version number of this copy.</extracomment>
        <translation>Имате %1.</translation>
    </message>
    <message>
        <source>Open Download Page</source>
        <translation>Отворете страницата за изтегляне</translation>
    </message>
    <message>
        <source>Couldn&apos;t save the update.

%1</source>
        <extracomment>%1 is the file error message.</extracomment>
        <translation>Актуализацията не можа да бъде запазена.

%1</translation>
    </message>
    <message>
        <source>Downloading Mervin PDF %1…</source>
        <extracomment>%1 is the version number being downloaded.</extracomment>
        <translation>Изтегля се Mervin PDF %1…</translation>
    </message>
    <message>
        <source>Cancel</source>
        <translation>Отказ</translation>
    </message>
    <message>
        <source>The update download failed.

%1</source>
        <extracomment>%1 is the network error message.</extracomment>
        <translation>Изтеглянето на актуализацията не бе успешно.

%1</translation>
    </message>
    <message>
        <source>The downloaded update failed its integrity check and was discarded.</source>
        <translation>Изтеглената актуализация не успя да провери целостта си и беше отхвърлена.</translation>
    </message>
    <message>
        <source>Couldn&apos;t save the update.</source>
        <translation>Актуализацията не можа да бъде запазена.</translation>
    </message>
    <message>
        <source>Couldn&apos;t start the installer. It is saved at:

%1

Run it to finish updating.</source>
        <extracomment>%1 is the path of the downloaded installer.</extracomment>
        <translation>Инсталационната програма не може да стартира. Той е запазен на:

%1

Стартирайте го, за да завършите актуализирането.</translation>
    </message>
    <message>
        <source>Couldn&apos;t install the update. %1

The new AppImage is saved at:

%2</source>
        <extracomment>%1 is the reason, %2 the path of the downloaded AppImage file.</extracomment>
        <translation>Актуализацията не можа да се инсталира. %1

Новото AppImage се записва на:

%2</translation>
    </message>
    <message>
        <source>Installing Mervin PDF %1…</source>
        <extracomment>%1 is the version number being installed.</extracomment>
        <translation>Инсталиране на Mervin PDF %1...</translation>
    </message>
    <message>
        <source>The update could not be installed.

%1</source>
        <extracomment>%1 is the package manager&apos;s error output.</extracomment>
        <translation>Актуализацията не можа да бъде инсталирана.

%1</translation>
    </message>
    <message>
        <source>This copy is not running as an AppImage.</source>
        <extracomment>AppImage is a Linux package format; keep the name as it is.</extracomment>
        <translation>Това копие не се изпълнява като AppImage.</translation>
    </message>
    <message>
        <source>Couldn&apos;t write to %1.</source>
        <extracomment>%1 is a folder path.</extracomment>
        <translation>Не може да се пише на %1.</translation>
    </message>
    <message>
        <source>Couldn&apos;t replace %1.</source>
        <extracomment>%1 is the path of the running AppImage file.</extracomment>
        <translation>%1 не можа да бъде заменен.</translation>
    </message>
</context>
<context>
    <name>mervin::ViewerWidget</name>
    <message>
        <source>Copy value</source>
        <translation>Копирай стойността</translation>
    </message>
    <message>
        <source>Delete measurement</source>
        <translation>Изтриване на измерването</translation>
    </message>
    <message>
        <source>Draw a line over a known dimension…</source>
        <extracomment>Prompt while calibrating: the user draws a line over a dimension whose real length they know, then types that length.</extracomment>
        <translation>Начертайте линия над известно измерение...</translation>
    </message>
    <message>
        <source>No scale - Calibrate (paper · %1)</source>
        <extracomment>Shown when the page has no drawing scale. %1 is the unit symbol, such as mm. Values are measured on the paper until the user calibrates.</extracomment>
        <translation>Няма мащаб на чертежа - Калибриране (хартия · %1)</translation>
    </message>
    <message>
        <source>Scale %1 · %2 · calibrated</source>
        <extracomment>%1 is the drawing scale, such as 1:100. %2 is the unit symbol, such as mm. The user set the scale by drawing a line of known length.</extracomment>
        <translation>Мащаб %1 · %2 · калибриран</translation>
    </message>
    <message>
        <source>Scale %1 · %2 · manual</source>
        <extracomment>%1 is the drawing scale, such as 1:100. %2 is the unit symbol, such as mm. The user typed the scale.</extracomment>
        <translation>Мащаб %1 · %2 · ръчен</translation>
    </message>
    <message>
        <source>Scale %1 · %2</source>
        <extracomment>%1 is the drawing scale, such as 1:100, read from the PDF. %2 is the unit symbol, such as mm.</extracomment>
        <translation>Мащаб %1 · %2</translation>
    </message>
</context>
<context>
    <name>mervin::WindowManager</name>
    <message>
        <source>Show Mervin</source>
        <extracomment>Tray icon menu item: show the windows hidden in the tray.</extracomment>
        <translation>Покажи Mervin</translation>
    </message>
    <message>
        <source>Quit Mervin</source>
        <extracomment>Tray icon menu item: close every window and exit.</extracomment>
        <translation>Изход от Mervin</translation>
    </message>
    <message>
        <source>Mervin PDF</source>
        <extracomment>Tooltip of the tray icon. The product name.</extracomment>
        <translation>Mervin PDF</translation>
    </message>
    <message>
        <source>Document remains loaded</source>
        <extracomment>Title of a tray notification: an inactive document could not be unloaded from memory.</extracomment>
        <translation>Документът остава зареден</translation>
    </message>
</context>
<context>
    <name>mervin::measurementPages</name>
    <message>
        <source>Invalid measurement metadata.</source>
        <extracomment>Page operation error. The measurements Mervin saved in this PDF are damaged.</extracomment>
        <translation>Невалидни метаданни за измерване.</translation>
    </message>
    <message>
        <source>Unsupported measurement metadata.</source>
        <extracomment>Page operation error. The measurements Mervin saved in this PDF are damaged or come from a newer version.</extracomment>
        <translation>Неподдържани метаданни за измерване.</translation>
    </message>
</context>
</TS>
