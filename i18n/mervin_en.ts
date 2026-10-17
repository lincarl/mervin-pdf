<?xml version="1.0" encoding="utf-8"?>
<!DOCTYPE TS>
<TS version="2.1" language="en" sourcelanguage="en">
<context>
    <name>MainWindow</name>
    <message numerus="yes">
        <source>%n file(s) in %1 already exist. Split wrote nothing.</source>
        <extracomment>Tray message after Split found that some of the files it would write, such as plan-002.pdf, already exist while its window was hidden in the tray, so it could not ask whether to replace them. %n is the number of those files, %1 the folder Split writes to. Split wrote no file.</extracomment>
        <translation>
            <numerusform>%n file in %1 already exists. Split wrote nothing.</numerusform>
            <numerusform>%n files in %1 already exist. Split wrote nothing.</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) in %1 already exist. Replace them?</source>
        <extracomment>Asked after Split found that some of the files it would write, such as plan-002.pdf, already exist. %n is the number of those files, %1 the folder Split writes to. Nothing has been replaced yet. Yes replaces those files and writes the others; No writes no file.</extracomment>
        <translation>
            <numerusform>%n file in %1 already exists. Replace it?</numerusform>
            <numerusform>%n files in %1 already exist. Replace them?</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Wrote %n file(s) to:
%1</source>
        <extracomment>%1 is the folder the files were written to.</extracomment>
        <translation>
            <numerusform>Wrote %n file to:
%1</numerusform>
            <numerusform>Wrote %n files to:
%1</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::ExtractPlan</name>
    <message numerus="yes">
        <source>Result: %n of %1 page(s) remain (%2).</source>
        <extracomment>Result line of Delete pages, under the page strip. %n is how many pages the new file keeps, %1 the document&apos;s page count, %2 the pages that remain as ranges, like &quot;1, 3-4&quot;. In English the verb follows %n: &quot;1 of 6 pages remains&quot;, &quot;3 of 6 pages remain&quot;.</extracomment>
        <translation>
            <numerusform>Result: %n of %1 pages remains (%2).</numerusform>
            <numerusform>Result: %n of %1 pages remain (%2).</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Result: %n page(s), in the order shown.</source>
        <translation>
            <numerusform>Result: %n page, in the order shown.</numerusform>
            <numerusform>Result: %n pages, in the order shown.</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Pages %1 (%n page(s)). Click to show them.</source>
        <extracomment>Tooltip of a tile standing in for hidden pages: %1 is their range, like &quot;14-30&quot;.</extracomment>
        <translation>
            <numerusform>Pages %1 (%n page). Click to show them.</numerusform>
            <numerusform>Pages %1 (%n pages). Click to show them.</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Deleted %n page(s) in a copy saved as %1</source>
        <extracomment>Status bar message after Delete pages wrote its new file. %n is how many pages it left out, %1 the new file&apos;s name. The original is unchanged.</extracomment>
        <translation>
            <numerusform>Deleted %n page in a copy saved as %1</numerusform>
            <numerusform>Deleted %n pages in a copy saved as %1</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Extracted %n page(s) to %1</source>
        <extracomment>%1 is the new file&apos;s name.</extracomment>
        <translation>
            <numerusform>Extracted %n page to %1</numerusform>
            <numerusform>Extracted %n pages to %1</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::ManageLanguagesDialog</name>
    <message numerus="yes">
        <source>%n best-quality model(s) available</source>
        <extracomment>%n is the number of OCR language models listed.</extracomment>
        <translation>
            <numerusform>%n best-quality model available</numerusform>
            <numerusform>%n best-quality models available</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::MergeDialog</name>
    <message numerus="yes">
        <source>These files cannot be merged:

%1</source>
        <extracomment>%1 is a list of files, one per line. The number of files picks the singular or plural form.</extracomment>
        <translation>
            <numerusform>This file cannot be merged:

%1</numerusform>
            <numerusform>These files cannot be merged:

%1</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::MergePlan</name>
    <message numerus="yes">
        <source>%n file(s)</source>
        <extracomment>Goes into %1 of &quot;Result: %n page(s) from %1, in the order shown.&quot;, so use the form that fits after &quot;from&quot;.</extracomment>
        <translation>
            <numerusform>%n file</numerusform>
            <numerusform>%n files</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Result: %n page(s) from %1, in the order shown.</source>
        <extracomment>%1 is the number of files, like &quot;4 files&quot;.</extracomment>
        <translation>
            <numerusform>Result: %n page from %1, in the order shown.</numerusform>
            <numerusform>Result: %n pages from %1, in the order shown.</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::PageOps</name>
    <message numerus="yes">
        <source>It has %n page(s); page %1 does not exist.</source>
        <extracomment>Merge or extract error, shown below a line that names the file. %n is the file&apos;s page count, %1 the missing page number.</extracomment>
        <translation>
            <numerusform>It has %n page; page %1 does not exist.</numerusform>
            <numerusform>It has %n pages; page %1 does not exist.</numerusform>
        </translation>
    </message>
</context>
<context>
    <name>mervin::RecentFilesPanel</name>
    <message numerus="yes">
        <source>%n day(s) ago</source>
        <extracomment>When a recent file was last opened. Used from 2 days on.</extracomment>
        <translation>
            <numerusform>%n day ago</numerusform>
            <numerusform>%n days ago</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n page(s)</source>
        <translation>
            <numerusform>%n page</numerusform>
            <numerusform>%n pages</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Stopped, %n file(s) found</source>
        <translation>
            <numerusform>Stopped, %n file found</numerusform>
            <numerusform>Stopped, %n files found</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n file(s) found</source>
        <translation>
            <numerusform>%n file found</numerusform>
            <numerusform>%n files found</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>%n result(s)</source>
        <translation>
            <numerusform>%n result</numerusform>
            <numerusform>%n results</numerusform>
        </translation>
    </message>
    <message numerus="yes">
        <source>Your last %n opened document(s)</source>
        <extracomment>Status bar summary of the Recent list.</extracomment>
        <translation>
            <numerusform>Your last %n opened document</numerusform>
            <numerusform>Your last %n opened documents</numerusform>
        </translation>
    </message>
</context>
</TS>
