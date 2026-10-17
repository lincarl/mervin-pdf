#include "i18n/UiLanguage.h"
#include "io/DestinationConsent.h"
#include "render/Document.h"
#include "render/RenderEngine.h"
#include "security/PageOps.h"
#include "security/QpdfService.h"

#include <qpdf/Buffer.hh>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>
#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFObjectHandle.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>
#include <qpdf/QPDFWriter.hh>

#include <QBuffer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QImage>
#include <QTemporaryDir>
#include <QTest>

#include <cctype>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

using mervin::PageOps;
using mervin::QpdfService;

Q_DECLARE_METATYPE(QpdfService::Algorithm)
Q_DECLARE_METATYPE(qpdf_r3_print_e)

namespace {

// Text that only page 2 of the delete fixtures carries, in its content and in whatever
// refers to it.
const std::string kSecret = "PAGE2SECRET";

QPDFObjectHandle parse(const std::string &text)
{
    return QPDFObjectHandle::parse(text);
}

// What refers to page 2 of a delete fixture from outside the page tree. Each case on its
// own kept page 2's content in a Delete Pages copy before the cleanup.
enum class Reference {
    Outline,         // a bookmark to page 2 whose title names the secret
    NamedOutline,    // the same through a GoTo action and a named destination
    NestedOutline,   // such a bookmark below one that stays, and one to page 2 above one that stays
    Link,            // a link on page 1 to page 2
    Reply,           // a reply on page 1 to a note on page 2, each with a popup
    Field,           // a text field whose only widget is on page 2
    CalculatedField, // a parent field in /CO whose only widget is on page 2
    ChildField,      // a child field on page 2 beside a child field on page 1
    SharedField,     // one field with widgets on pages 1 and 2
    Xfa,             // a field on page 2 of a form that also carries XFA data
    Structure,       // a figure element on page 2 with alternate text
    NestedStructure, // a section only on page 2, and a paragraph across pages 2 and 3
    TaggedWidget,    // an /OBJR structure element for the page 2 widget of a tagged form
    ResetButton,     // a reset button on page 1 that names a field with a child on each page
    ChildFieldNoForm, // ChildField in a file without /AcroForm, as Extract wrote before forms
    SharedResources, // one /Resources for all pages, with the image that only page 2 draws
    InheritedResources, // the same /Resources on /Pages instead of on the pages
    OwnResources,    // a /Resources of its own on each page, each listing every page's image
    DirectOutline,   // a bookmark to page 2, then a direct one to page 1, then one to page 3
    ClosedOutline,   // closed bookmarks with children on pages 1 and 2, and only on page 2
    SharedWidget,    // a field whose one widget pages 1 and 2 both list
    WidgetlessField, // a field that no page shows, beside a field on page 2
    ReplyInForm,     // Reply in a file with a form, so copies go through transformAnnotations
    ResetByName,     // a reset button on page 1 that names its page's field by text
    SubmitButtons,   // buttons on page 1 that submit or reset every field, or all but some
};

// A page that shows `text` in two marked-content sequences, MCID 0 and 1.
QPDFObjectHandle textPage(QPDF &q, const std::string &text, int width = 612)
{
    QPDFObjectHandle page = parse("<< /Type /Page /Resources << /Font << /F1 << /Type /Font "
                                  "/Subtype /Type1 /BaseFont /Helvetica >> >> >> >>");
    page.replaceKey("/MediaBox", parse("[0 0 " + std::to_string(width) + " 792]"));
    page.replaceKey("/Contents",
                    q.newStream("/P <</MCID 0>> BDC BT /F1 12 Tf 72 700 Td (" + text
                                + ") Tj ET EMC /P <</MCID 1>> BDC BT /F1 12 Tf 72 680 Td ("
                                + text + " again) Tj ET EMC"));
    return q.makeIndirectObject(page);
}

// A normal appearance that shows `text`.
QPDFObjectHandle appearance(QPDF &q, const std::string &text)
{
    QPDFObjectHandle stream = q.newStream("BT /F1 12 Tf 2 10 Td (" + text + ") Tj ET");
    stream.getDict().replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    stream.getDict().replaceKey("/Subtype", QPDFObjectHandle::newName("/Form"));
    stream.getDict().replaceKey("/BBox", parse("[0 0 228 30]"));
    QPDFObjectHandle ap = QPDFObjectHandle::newDictionary();
    ap.replaceKey("/N", stream);
    return ap;
}

// Adds `annot` to the page's /Annots and points its /P at the page.
void annotate(QPDFObjectHandle page, QPDFObjectHandle annot)
{
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (!annots.isArray())
        annots = page.replaceKeyAndGetNew("/Annots", QPDFObjectHandle::newArray());
    annots.appendItem(annot);
    annot.replaceKey("/P", page);
}

// A widget on `page` with the given extra keys, listed in its /Annots.
QPDFObjectHandle widget(QPDF &q, QPDFObjectHandle page, const std::string &keys = {})
{
    QPDFObjectHandle annot = q.makeIndirectObject(
        parse("<< /Type /Annot /Subtype /Widget /Rect [72 600 300 630] /F 4 " + keys + " >>"));
    annotate(page, annot);
    return annot;
}

// Makes `kids` the field children of `parent`.
void adopt(QPDFObjectHandle parent, const std::vector<QPDFObjectHandle> &kids)
{
    parent.replaceKey("/Kids", QPDFObjectHandle::newArray(kids));
    for (QPDFObjectHandle kid : kids)
        kid.replaceKey("/Parent", parent);
}

// Installs an AcroForm with `fields` that needs appearances, and returns it.
QPDFObjectHandle installForm(QPDF &q, const std::vector<QPDFObjectHandle> &fields)
{
    QPDFObjectHandle form = q.makeIndirectObject(
        parse("<< /NeedAppearances true /DA (/Helv 0 Tf 0 g) /DR << /Font << /Helv << /Type "
              "/Font /Subtype /Type1 /BaseFont /Helvetica >> >> >> >>"));
    form.replaceKey("/Fields", QPDFObjectHandle::newArray(fields));
    q.getRoot().replaceKey("/AcroForm", form);
    return form;
}

// [page /Fit]
QPDFObjectHandle fit(QPDFObjectHandle page)
{
    QPDFObjectHandle dest = QPDFObjectHandle::newArray();
    dest.appendItem(page);
    dest.appendItem(QPDFObjectHandle::newName("/Fit"));
    return dest;
}

// An outline item with `title` that goes to `page`.
QPDFObjectHandle bookmark(QPDF &q, const std::string &title, QPDFObjectHandle page)
{
    QPDFObjectHandle item = q.makeIndirectObject(QPDFObjectHandle::newDictionary());
    item.replaceKey("/Title", QPDFObjectHandle::newString(title));
    item.replaceKey("/Dest", fit(page));
    return item;
}

// Links `items` below `parent`, open, with /Parent, /Prev, /Next and /Count. Link children
// before their parents, as a parent's /Count includes theirs.
void outline(QPDFObjectHandle parent, const std::vector<QPDFObjectHandle> &items)
{
    int count = 0;
    for (size_t i = 0; i < items.size(); ++i) {
        QPDFObjectHandle item = items[i];
        item.replaceKey("/Parent", parent);
        if (i > 0)
            item.replaceKey("/Prev", items[i - 1]);
        if (i + 1 < items.size())
            item.replaceKey("/Next", items[i + 1]);
        QPDFObjectHandle below = item.getKey("/Count");
        count += 1 + (below.isInteger() ? std::max(0, below.getIntValueAsInt()) : 0);
    }
    parent.replaceKey("/First", items.front());
    parent.replaceKey("/Last", items.back());
    parent.replaceKey("/Count", QPDFObjectHandle::newInteger(count));
}

// A structure element of type `type` below `parent`.
QPDFObjectHandle element(QPDF &q, const std::string &type, QPDFObjectHandle parent,
                         const std::string &keys = {})
{
    QPDFObjectHandle elem = q.makeIndirectObject(parse("<< /Type /StructElem " + keys + " >>"));
    elem.replaceKey("/S", QPDFObjectHandle::newName(type));
    elem.replaceKey("/P", parent);
    return elem;
}

// Builds three text pages, where only page 2 shows kSecret, and refers to page 2 as
// `reference` says. Pages 1 and 3 show "page one" and "page three".
void makeReferencedPdf(const QString &path, Reference reference)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper dh(q);
    std::vector<QPDFObjectHandle> pages{textPage(q, "page one"), textPage(q, kSecret),
                                        textPage(q, "page three")};
    for (size_t i = 0; i < pages.size(); ++i) {
        pages[i].replaceKey("/StructParents", QPDFObjectHandle::newInteger(int(i)));
        dh.addPage(QPDFPageObjectHelper(pages[i]), false);
    }
    QPDFObjectHandle root = q.getRoot();
    QPDFObjectHandle outlines = q.makeIndirectObject(parse("<< /Type /Outlines >>"));
    // The tagged cases share a Document element with one element per page, each marking
    // both of its page's sequences, and a ParentTree for them.
    QPDFObjectHandle tree = q.makeIndirectObject(parse("<< /Type /StructTreeRoot >>"));
    QPDFObjectHandle document = element(q, "/Document", tree);
    std::vector<QPDFObjectHandle> perPage;
    for (size_t i = 0; i < pages.size(); ++i) {
        perPage.push_back(element(q, i == 1 ? "/Figure" : "/P", document, "/K [0 1]"));
        perPage.back().replaceKey("/Pg", pages[i]);
    }
    std::map<int, QPDFObjectHandle> parents;
    for (int i = 0; i < 3; ++i)
        parents[i] = QPDFObjectHandle::newArray({perPage[i], perPage[i]});
    std::vector<QPDFObjectHandle> documentKids = perPage;
    bool tagged = false;

    switch (reference) {
    case Reference::Outline:
        outline(outlines, {bookmark(q, "Page one", pages[0]),
                           bookmark(q, kSecret + " bookmark", pages[1])});
        root.replaceKey("/Outlines", outlines);
        break;
    case Reference::NamedOutline: {
        // The destination names of page 2 name the secret, those of page 1 stay.
        QPDFObjectHandle names = QPDFObjectHandle::newArray(
            {QPDFObjectHandle::newString(kSecret + " chapter"), fit(pages[1]),
             QPDFObjectHandle::newString("pageone"), fit(pages[0])});
        root.replaceKey("/Names", parse("<< /Dests << >> >>"));
        root.getKey("/Names").getKey("/Dests").replaceKey("/Names", names);
        root.replaceKey("/Dests", q.makeIndirectObject(QPDFObjectHandle::newDictionary()));
        root.getKey("/Dests").replaceKey("/" + kSecret + "dest", fit(pages[1]));
        root.getKey("/Dests").replaceKey("/pageone", fit(pages[0]));
        QPDFObjectHandle named = bookmark(q, kSecret + " bookmark", pages[1]);
        named.removeKey("/Dest");
        named.replaceKey("/A", parse("<< /S /GoTo >>"));
        named.getKey("/A").replaceKey("/D", QPDFObjectHandle::newString(kSecret + " chapter"));
        outline(outlines, {bookmark(q, "Page one", pages[0]), named});
        root.replaceKey("/Outlines", outlines);
        break;
    }
    case Reference::NestedOutline: {
        QPDFObjectHandle chapter = bookmark(q, "Chapter", pages[1]);
        QPDFObjectHandle section = bookmark(q, "Section", pages[0]);
        outline(chapter, {bookmark(q, "Kept child", pages[0])});
        outline(section, {bookmark(q, kSecret + " child", pages[1])});
        outline(outlines, {chapter, section});
        root.replaceKey("/Outlines", outlines);
        break;
    }
    case Reference::Link: {
        QPDFObjectHandle link = q.makeIndirectObject(
            parse("<< /Type /Annot /Subtype /Link /Rect [72 600 300 630] /Border [0 0 0] >>"));
        link.replaceKey("/Dest", fit(pages[1]));
        annotate(pages[0], link);
        break;
    }
    case Reference::Reply:
    case Reference::ReplyInForm: {
        QPDFObjectHandle note = q.makeIndirectObject(
            parse("<< /Type /Annot /Subtype /Text /Rect [72 600 92 620] >>"));
        note.replaceKey("/Contents", QPDFObjectHandle::newString(kSecret + " note"));
        annotate(pages[1], note);
        QPDFObjectHandle reply = q.makeIndirectObject(
            parse("<< /Type /Annot /Subtype /Text /Rect [72 600 92 620] /Contents (Reply) >>"));
        reply.replaceKey("/IRT", note);
        annotate(pages[0], reply);
        for (size_t i : {size_t(0), size_t(1)}) {
            QPDFObjectHandle markup = i == 0 ? reply : note;
            QPDFObjectHandle popup = q.makeIndirectObject(
                parse("<< /Type /Annot /Subtype /Popup /Rect [100 600 300 700] >>"));
            popup.replaceKey("/Parent", markup);
            markup.replaceKey("/Popup", popup);
            annotate(pages[i], popup);
        }
        if (reference == Reference::ReplyInForm)
            installForm(q, {widget(q, pages[0], "/FT /Tx /T (name) /V (kept name)")});
        break;
    }
    case Reference::Field:
        installForm(q, {widget(q, pages[1], "/FT /Tx /T (secret) /V (" + kSecret + ")")});
        break;
    case Reference::CalculatedField: {
        QPDFObjectHandle first = widget(q, pages[0], "/FT /Tx /T (first) /V (one)");
        QPDFObjectHandle total = q.makeIndirectObject(parse("<< /FT /Tx /T (total) >>"));
        total.replaceKey("/V", QPDFObjectHandle::newString(kSecret));
        adopt(total, {widget(q, pages[1])});
        installForm(q, {first, total}).replaceKey("/CO", QPDFObjectHandle::newArray({first, total}));
        break;
    }
    case Reference::ChildField: {
        QPDFObjectHandle address = q.makeIndirectObject(parse("<< /T (address) >>"));
        QPDFObjectHandle street =
            q.makeIndirectObject(parse("<< /FT /Tx /T (street) /V (kept street) >>"));
        QPDFObjectHandle secret = q.makeIndirectObject(parse("<< /FT /Tx /T (secret) >>"));
        secret.replaceKey("/V", QPDFObjectHandle::newString(kSecret));
        adopt(street, {widget(q, pages[0])});
        adopt(secret, {widget(q, pages[1])});
        adopt(address, {street, secret});
        installForm(q, {address});
        break;
    }
    case Reference::SharedField: {
        QPDFObjectHandle shared =
            q.makeIndirectObject(parse("<< /FT /Tx /T (shared) /V (shared value) >>"));
        QPDFObjectHandle first = widget(q, pages[0]);
        first.replaceKey("/AP", appearance(q, "shared value"));
        QPDFObjectHandle second = widget(q, pages[1]);
        second.replaceKey("/AP", appearance(q, kSecret));
        adopt(shared, {first, second});
        installForm(q, {shared});
        break;
    }
    case Reference::Xfa:
        installForm(q, {widget(q, pages[0], "/FT /Tx /T (first) /V (one)"),
                        widget(q, pages[1], "/FT /Tx /T (secret) /V (two)")})
            .replaceKey("/XFA", q.newStream("<xdp><datasets>" + kSecret + "</datasets></xdp>"));
        break;
    case Reference::Structure:
        perPage[1].replaceKey("/Alt", QPDFObjectHandle::newString(kSecret + " figure"));
        tagged = true;
        break;
    case Reference::NestedStructure: {
        // The section's figure marks page 2's first sequence; the paragraph marks the
        // second sequence of pages 2 and 3.
        QPDFObjectHandle section = element(q, "/Sect", document, "/ID (section-id)");
        section.replaceKey("/T", QPDFObjectHandle::newString(kSecret + " section"));
        QPDFObjectHandle figure = element(q, "/Figure", section, "/K 0 /Alt (figure)");
        figure.replaceKey("/Pg", pages[1]);
        section.replaceKey("/K", figure);
        QPDFObjectHandle across = element(q, "/P", document);
        across.replaceKey("/K", QPDFObjectHandle::newArray(
            {parse("<< /Type /MCR /MCID 1 >>"), parse("<< /Type /MCR /MCID 1 >>")}));
        across.getKey("/K").getArrayItem(0).replaceKey("/Pg", pages[1]);
        across.getKey("/K").getArrayItem(1).replaceKey("/Pg", pages[2]);
        perPage[2].replaceKey("/K", QPDFObjectHandle::newInteger(0));
        documentKids = {perPage[0], section, across, perPage[2]};
        parents[1] = QPDFObjectHandle::newArray({figure, across});
        parents[2] = QPDFObjectHandle::newArray({perPage[2], across});
        tree.replaceKey("/IDTree", parse("<< /Names [(section-id) null] >>"));
        tree.getKey("/IDTree").getKey("/Names").setArrayItem(1, section);
        tagged = true;
        break;
    }
    case Reference::TaggedWidget: {
        QPDFObjectHandle field = widget(q, pages[1], "/FT /Tx /T (secret) /StructParent 3");
        field.replaceKey("/V", QPDFObjectHandle::newString(kSecret));
        field.replaceKey("/AP", appearance(q, kSecret));
        installForm(q, {field});
        QPDFObjectHandle form = element(q, "/Form", document);
        form.replaceKey("/Pg", pages[1]);
        form.replaceKey("/K", parse("<< /Type /OBJR >>"));
        form.getKey("/K").replaceKey("/Obj", field);
        documentKids = {perPage[0], perPage[1], form, perPage[2]};
        parents[3] = form;
        tagged = true;
        break;
    }
    case Reference::ResetButton: {
        QPDFObjectHandle form1 = q.makeIndirectObject(parse("<< /FT /Tx /T (form1) >>"));
        QPDFObjectHandle one = q.makeIndirectObject(parse("<< /T (p1) /V (kept one) >>"));
        QPDFObjectHandle two = q.makeIndirectObject(parse("<< /T (p2) >>"));
        two.replaceKey("/V", QPDFObjectHandle::newString(kSecret));
        adopt(one, {widget(q, pages[0])});
        adopt(two, {widget(q, pages[1])});
        adopt(form1, {one, two});
        QPDFObjectHandle reset = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (reset)");
        reset.replaceKey("/A", parse("<< /S /ResetForm /Fields [] >>"));
        reset.getKey("/A").getKey("/Fields").appendItem(form1);
        installForm(q, {form1, reset});
        break;
    }
    case Reference::SharedResources:
    case Reference::InheritedResources:
    case Reference::OwnResources: {
        // Each page draws its own image, and only page 2's image data is the secret.
        QPDFObjectHandle images = QPDFObjectHandle::newDictionary();
        const std::string data[] = {"IMAGEONE", kSecret, "IMAGETHREE"};
        for (size_t i = 0; i < pages.size(); ++i) {
            QPDFObjectHandle image = q.newStream(data[i]);
            image.replaceDict(parse("<< /Type /XObject /Subtype /Image /Height 1 /ColorSpace "
                                    "/DeviceGray /BitsPerComponent 8 >>"));
            image.getDict().replaceKey("/Width", QPDFObjectHandle::newInteger(int(data[i].size())));
            const std::string name = "/Im" + std::to_string(i + 1);
            images.replaceKey(name, image);
            QPDFObjectHandle draw = q.newStream("q 200 0 0 20 72 400 cm " + name + " Do Q");
            pages[i].replaceKey("/Contents",
                                QPDFObjectHandle::newArray({pages[i].getKey("/Contents"), draw}));
        }
        QPDFObjectHandle resources = pages[0].getKey("/Resources").shallowCopy();
        resources.replaceKey("/XObject", images);
        resources = q.makeIndirectObject(resources);
        for (QPDFObjectHandle page : pages) {
            if (reference == Reference::SharedResources)
                page.replaceKey("/Resources", resources);
            else if (reference == Reference::OwnResources)
                page.replaceKey("/Resources", resources.shallowCopy()); // direct, as copies
            else
                page.removeKey("/Resources");
        }
        if (reference == Reference::InheritedResources)
            root.getKey("/Pages").replaceKey("/Resources", resources);
        break;
    }
    case Reference::DirectOutline: {
        // ISO 32000 wants indirect items, but viewers follow a direct /Next as well.
        QPDFObjectHandle first = bookmark(q, kSecret + " bookmark", pages[1]);
        QPDFObjectHandle last = bookmark(q, "Page three", pages[2]);
        QPDFObjectHandle direct = QPDFObjectHandle::newDictionary();
        direct.replaceKey("/Title", QPDFObjectHandle::newString("Direct one"));
        direct.replaceKey("/Dest", fit(pages[0]));
        direct.replaceKey("/Next", last);
        for (QPDFObjectHandle item : {first, direct, last})
            item.replaceKey("/Parent", outlines);
        first.replaceKey("/Next", direct);
        outlines.replaceKey("/First", first);
        outlines.replaceKey("/Last", last);
        outlines.replaceKey("/Count", QPDFObjectHandle::newInteger(3));
        root.replaceKey("/Outlines", outlines);
        break;
    }
    case Reference::ClosedOutline: {
        // A closed chapter keeps one of its two children, a closed section loses its only one.
        QPDFObjectHandle chapter = bookmark(q, "Chapter", pages[0]);
        QPDFObjectHandle section = bookmark(q, "Section", pages[2]);
        outline(chapter, {bookmark(q, "Kept child", pages[0]), bookmark(q, kSecret + " child", pages[1])});
        outline(section, {bookmark(q, kSecret + " only child", pages[1])});
        chapter.replaceKey("/Count", QPDFObjectHandle::newInteger(-2));
        section.replaceKey("/Count", QPDFObjectHandle::newInteger(-1));
        outline(outlines, {chapter, section});
        root.replaceKey("/Outlines", outlines);
        break;
    }
    case Reference::SharedWidget: {
        // Extract wrote repeated pages before forms with one widget object on both copies.
        // This file also has a form, so the field must keep the widget as well.
        QPDFObjectHandle twice = widget(q, pages[0], "/FT /Tx /T (twice) /V (kept twice)");
        pages[1].replaceKey("/Annots", QPDFObjectHandle::newArray({twice}));
        installForm(q, {twice});
        break;
    }
    case Reference::WidgetlessField: {
        QPDFObjectHandle data = q.makeIndirectObject(parse("<< /FT /Tx /T (data) /V (kept data) >>"));
        installForm(q, {data, widget(q, pages[1], "/FT /Tx /T (secret) /V (" + kSecret + ")")});
        break;
    }
    case Reference::SubmitButtons: {
        // Without /Fields, or with the Exclude flag (bit 1 of /Flags), an action acts on
        // every field of the document, or on every field but the listed ones.
        QPDFObjectHandle name = widget(q, pages[0], "/FT /Tx /T (name) /V (kept name)");
        name.replaceKey("/AA", parse("<< /Bl << /S /ResetForm >> >>"));
        QPDFObjectHandle other = widget(q, pages[0], "/FT /Tx /T (other) /V (kept other)");
        const std::string all =
            "[(name) (other) (submit) (except) (none) (group) (exceptchild) (emptylist) (resetlater)]";
        other.replaceKey("/AA", parse("<< /Fo << /S /ResetForm /Fields " + all + " /Flags 1 >> >>"));
        QPDFObjectHandle submit = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (submit)");
        submit.replaceKey("/A", parse("<< /S /SubmitForm /F (https://example.invalid/form) /Flags 4 "
                                      "/Next << /S /ResetForm >> >>"));
        QPDFObjectHandle except = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (except)");
        except.replaceKey("/A", parse("<< /S /SubmitForm /F (https://example.invalid/form) "
                                      "/Fields [(name)] /Flags 1 /Next << /S /ResetForm /Fields " + all
                                      + " /Flags 1 /Next << /S /SubmitForm /F (https://example.invalid/"
                                        "form) /Fields [(other)] >> >> >>"));
        QPDFObjectHandle none = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (none)");
        none.replaceKey("/A", parse("<< /S /ResetForm /Fields " + all + " /Flags 1 /Next << /S "
                                    "/SubmitForm /F (https://example.invalid/form) /Fields [(name)] >> >>"));
        QPDFObjectHandle group = q.makeIndirectObject(parse("<< /FT /Tx /T (group) >>"));
        QPDFObjectHandle a = q.makeIndirectObject(parse("<< /T (a) /V (kept a) >>"));
        QPDFObjectHandle b = q.makeIndirectObject(parse("<< /T (b) /V (kept b) >>"));
        adopt(a, {widget(q, pages[0])});
        adopt(b, {widget(q, pages[0])});
        adopt(group, {a, b});
        QPDFObjectHandle exceptChild = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (exceptchild)");
        exceptChild.replaceKey("/A", parse("<< /S /SubmitForm /F (https://example.invalid/form) "
                                           "/Fields [(group.a)] /Flags 1 >>"));
        // An empty list, which viewers read as every field, and a list of a field on page 2.
        QPDFObjectHandle emptyList = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (emptylist)");
        emptyList.replaceKey("/A", parse("<< /S /ResetForm /Fields [] >>"));
        QPDFObjectHandle later = widget(q, pages[1], "/FT /Tx /T (later) /V (later value)");
        QPDFObjectHandle resetLater = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (resetlater)");
        resetLater.replaceKey("/A", parse("<< /S /ResetForm /Fields [(later)] >>"));
        resetLater.getKey("/A").getKey("/Fields").appendItem(later);
        installForm(q, {name, other, submit, except, none, group, exceptChild, emptyList, later,
                        resetLater});
        break;
    }
    case Reference::ResetByName: {
        // ISO 32000 lets /Fields and a hide action's /T name fields by text as well.
        QPDFObjectHandle name = widget(q, pages[0], "/FT /Tx /T (name) /V (kept name)");
        QPDFObjectHandle reset = widget(q, pages[0], "/FT /Btn /Ff 65536 /T (reset)");
        reset.replaceKey("/A", parse("<< /S /ResetForm /Fields [(name)] /Next << /S /Hide /T (name) "
                                     "/H false >> >>"));
        installForm(q, {name, reset});
        break;
    }
    case Reference::ChildFieldNoForm: {
        QPDFObjectHandle address = q.makeIndirectObject(parse("<< /T (address) >>"));
        QPDFObjectHandle street =
            q.makeIndirectObject(parse("<< /FT /Tx /T (street) /V (kept street) >>"));
        QPDFObjectHandle secret = q.makeIndirectObject(parse("<< /FT /Tx /T (secret) >>"));
        secret.replaceKey("/V", QPDFObjectHandle::newString(kSecret));
        adopt(street, {widget(q, pages[0])});
        adopt(secret, {widget(q, pages[1])});
        adopt(address, {street, secret});
        break;
    }
    }

    if (tagged) {
        document.replaceKey("/K", QPDFObjectHandle::newArray(documentKids));
        tree.replaceKey("/K", document);
        QPDFObjectHandle nums = QPDFObjectHandle::newArray();
        for (const auto &[key, value] : parents) {
            nums.appendItem(QPDFObjectHandle::newInteger(key));
            nums.appendItem(value);
        }
        QPDFObjectHandle parentTree = QPDFObjectHandle::newDictionary();
        parentTree.replaceKey("/Nums", nums);
        tree.replaceKey("/ParentTree", q.makeIndirectObject(parentTree));
        tree.replaceKey("/ParentTreeNextKey", QPDFObjectHandle::newInteger(4));
        root.replaceKey("/StructTreeRoot", tree);
        root.replaceKey("/MarkInfo", parse("<< /Marked true >>"));
    }
    const QByteArray outputName = path.toUtf8();
    QPDFWriter writer(q, outputName.constData());
    writer.write();
}

// Whether `object`, its direct parts or its decoded stream data contain `needle`.
bool holds(QPDFObjectHandle object, const std::string &needle)
{
    if (object.isString())
        return object.getStringValue().find(needle) != std::string::npos;
    if (object.isName())
        return object.getName().find(needle) != std::string::npos;
    if (object.isArray()) {
        for (QPDFObjectHandle item : object.aitems())
            if (!item.isIndirect() && holds(item, needle))
                return true;
        return false;
    }
    if (object.isStream()) {
        std::shared_ptr<Buffer> data;
        try {
            data = object.getStreamData(qpdf_dl_all);
        } catch (const std::exception &) {
            data = object.getRawStreamData();
        }
        const std::string bytes(reinterpret_cast<const char *>(data->getBuffer()),
                                data->getSize());
        return bytes.find(needle) != std::string::npos || holds(object.getDict(), needle);
    }
    if (object.isDictionary()) {
        for (const auto &[key, value] : object.ditems())
            if (key.find(needle) != std::string::npos || (!value.isIndirect() && holds(value, needle)))
                return true;
    }
    return false;
}

// Whether any object of `path` holds `needle` in a string, a name or decoded stream data.
// This looks at every object the file has, reachable or not.
bool pdfContains(const QString &path, const std::string &needle, const QString &password = {})
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    const QByteArray pw = password.toUtf8();
    q.processFile(name.constData(), password.isEmpty() ? nullptr : pw.constData());
    for (QPDFObjectHandle object : q.getAllObjects())
        if (holds(object, needle))
            return true;
    return false;
}

// Builds a form with one text field per page, named `prefix` plus the page number, on
// pages of the given widths. Its /AcroForm sets /NeedAppearances.
void makeFormPdf(const QString &path, const QList<int> &widths, const std::string &prefix)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper dh(q);
    std::vector<QPDFObjectHandle> fields;
    for (int i = 0; i < widths.size(); ++i) {
        QPDFObjectHandle page = textPage(q, "form page " + std::to_string(i + 1), widths.at(i));
        dh.addPage(QPDFPageObjectHelper(page), false);
        const std::string n = std::to_string(i + 1);
        fields.push_back(widget(q, page, "/FT /Tx /T (" + prefix + n + ") /V (value " + n
                                             + ") /DA (/Helv 12 Tf 0 g)"));
    }
    installForm(q, fields);
    const QByteArray outputName = path.toUtf8();
    QPDFWriter writer(q, outputName.constData());
    writer.write();
}

// The first reason `path` has no fillable form with `widgets` widgets, or an empty string.
// Each widget must belong to a field that /AcroForm /Fields reaches, point its /P at the
// page that lists it, and have its own top-level field, as each field of makeFormPdf has
// one widget. No two top-level fields may share a name, or they would fill in together.
// /NeedAppearances must still be true, and MuPDF must see the form.
QString formProblem(const QString &path, int widgets, const QString &password = {})
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    const QByteArray pw = password.toUtf8();
    q.processFile(name.constData(), password.isEmpty() ? nullptr : pw.constData());
    QPDFObjectHandle form = q.getRoot().getKey("/AcroForm");
    if (!form.isDictionary())
        return QStringLiteral("no /AcroForm");
    QPDFObjectHandle fields = form.getKey("/Fields");
    if (!fields.isArray() || fields.getArrayNItems() == 0)
        return QStringLiteral("no /Fields");
    if (!form.getKey("/NeedAppearances").isBool() || !form.getKey("/NeedAppearances").getBoolValue())
        return QStringLiteral("/NeedAppearances lost");
    std::set<std::string> names;
    for (QPDFObjectHandle field : fields.aitems())
        if (!names.insert(field.getKey("/T").getUTF8Value()).second)
            return QStringLiteral("two fields share a name");
    std::map<QPDFObjGen, QPDFObjGen> topOf; // every field and widget below /Fields
    std::vector<std::pair<QPDFObjectHandle, QPDFObjGen>> queue;
    for (QPDFObjectHandle field : fields.aitems())
        queue.emplace_back(field, field.getObjGen());
    while (!queue.empty()) {
        auto [field, top] = queue.back();
        queue.pop_back();
        if (!topOf.emplace(field.getObjGen(), top).second)
            continue;
        QPDFObjectHandle kids = field.getKey("/Kids");
        if (kids.isArray())
            for (QPDFObjectHandle kid : kids.aitems())
                queue.emplace_back(kid, top);
    }
    std::set<QPDFObjGen> seenWidgets, seenTops;
    int count = 0;
    for (QPDFObjectHandle page : q.getAllPages()) {
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (!annots.isArray())
            continue;
        for (QPDFObjectHandle annot : annots.aitems()) {
            if (!annot.getKey("/Subtype").isNameAndEquals("/Widget"))
                continue;
            ++count;
            auto top = topOf.find(annot.getObjGen());
            if (top == topOf.end())
                return QStringLiteral("a widget is not below /Fields");
            if (!seenWidgets.insert(annot.getObjGen()).second)
                return QStringLiteral("two pages share a widget");
            if (!seenTops.insert(top->second).second)
                return QStringLiteral("two widgets share a field");
            if (annot.getKey("/P").getObjGen() != page.getObjGen())
                return QStringLiteral("a widget's /P is another page");
        }
    }
    if (count != widgets)
        return QStringLiteral("%1 widgets").arg(count);
    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(path, password, &err, nullptr);
    if (!doc)
        return err;
    if (!doc->hasForm())
        return QStringLiteral("Document::hasForm() is false");
    return {};
}

// Each top-level form field of `path` as its name and the pages that its widgets point at
// with /P, for example "shared 1 2".
QStringList fieldPages(const QString &path)
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    q.processFile(name.constData());
    std::map<QPDFObjGen, int> pageNumber;
    for (QPDFObjectHandle page : q.getAllPages())
        pageNumber.emplace(page.getObjGen(), int(pageNumber.size()) + 1);
    QStringList lines;
    QPDFObjectHandle form = q.getRoot().getKey("/AcroForm");
    if (!form.isDictionary())
        return lines;
    for (QPDFObjectHandle field : form.getKey("/Fields").aitems()) {
        QString line = QString::fromStdString(field.getKey("/T").getUTF8Value());
        QPDFObjectHandle kids = field.getKey("/Kids");
        for (QPDFObjectHandle widget : kids.isArray() ? kids.getArrayAsVector()
                                                      : std::vector<QPDFObjectHandle>{field})
            line += QStringLiteral(" %1").arg(pageNumber[widget.getKey("/P").getObjGen()]);
        lines << line;
    }
    return lines;
}

// The first reason the comment "Reply" of page `index` of `path` is not a comment of its own
// on that page, with its popup there too, or an empty string.
QString replyProblem(const QString &path, int index)
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    q.processFile(name.constData());
    QPDFObjectHandle page = q.getAllPages().at(size_t(index));
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (!annots.isArray())
        return QStringLiteral("no annotations");
    std::set<QPDFObjGen> listed;
    for (QPDFObjectHandle annot : annots.aitems())
        listed.insert(annot.getObjGen());
    for (QPDFObjectHandle annot : annots.aitems()) {
        QPDFObjectHandle contents = annot.getKey("/Contents");
        if (!contents.isString() || contents.getUTF8Value() != "Reply")
            continue;
        if (annot.getKey("/IRT").isDictionary())
            return QStringLiteral("the reply still answers a note");
        QPDFObjectHandle popup = annot.getKey("/Popup");
        if (!popup.isDictionary() || !listed.count(popup.getObjGen()))
            return QStringLiteral("the reply's popup is not on its page");
        if (popup.getKey("/Parent").getObjGen() != annot.getObjGen())
            return QStringLiteral("the popup belongs to another annotation");
        return {};
    }
    return QStringLiteral("no reply");
}

// For each reset button of `path`, in page order, the first field that it names and the
// field that the hide action in its /Next names, for example "name name".
QStringList resetTargets(const QString &path)
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    q.processFile(name.constData());
    QStringList targets;
    for (QPDFObjectHandle page : q.getAllPages()) {
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (!annots.isArray())
            continue;
        for (QPDFObjectHandle annot : annots.aitems()) {
            QPDFObjectHandle action = annot.getKey("/A");
            if (!action.isDictionary() || !action.getKey("/S").isNameAndEquals("/ResetForm"))
                continue;
            QPDFObjectHandle first = action.getKey("/Fields").getArrayItem(0);
            QPDFObjectHandle hidden = action.getKey("/Next").getKey("/T");
            targets << QStringLiteral("%1 %2").arg(
                first.isString() ? QString::fromStdString(first.getUTF8Value()) : QStringLiteral("?"),
                hidden.isString() ? QString::fromStdString(hidden.getUTF8Value()) : QStringLiteral("?"));
        }
    }
    return targets;
}

// The type of the action under `keys` of top-level field `field` of `path`, and what it
// acts on: "all" without /Fields, otherwise the fully qualified names that /Fields lists,
// after "but" when the Exclude flag is set. For example "ResetForm: but name", or
// "no action".
QString actionScope(const QString &path, const std::string &field, const QStringList &keys)
{
    QPDF q;
    const QByteArray name = path.toUtf8();
    q.processFile(name.constData());
    for (QPDFObjectHandle top : q.getRoot().getKey("/AcroForm").getKey("/Fields").aitems()) {
        if (!top.getKey("/T").isString() || top.getKey("/T").getUTF8Value() != field)
            continue;
        QPDFObjectHandle action = top;
        for (const QString &key : keys)
            action = action.isDictionary() ? action.getKey(key.toStdString())
                                           : QPDFObjectHandle::newNull();
        if (!action.isDictionary())
            return QStringLiteral("no action");
        const QString type = QString::fromStdString(action.getKey("/S").getName()).mid(1) + u": ";
        QPDFObjectHandle fields = action.getKey("/Fields");
        if (!fields.isArray())
            return type + QStringLiteral("all");
        QStringList names;
        for (QPDFObjectHandle item : fields.aitems())
            names << QString::fromStdString(
                item.isString()       ? item.getUTF8Value()
                : item.isDictionary() ? QPDFFormFieldObjectHelper(item).getFullyQualifiedName()
                                      : std::string("null"));
        QPDFObjectHandle flags = action.getKey("/Flags");
        const bool exclude = flags.isInteger() && (flags.getIntValue() & 1);
        return type + (exclude ? QStringLiteral("but ") : QString()) + names.join(u' ');
    }
    return QStringLiteral("no field");
}

// Rewrites `path` with `blob` as its /Mervin_Measurements, as a string or as the JSON of a
// stream.
void setMeasurementBlob(const QString &path, const std::string &blob, bool stream)
{
    const QString copy = path + QStringLiteral(".tmp");
    {
        QPDF q; // closes the source before it is replaced, which Windows needs
        const QByteArray name = path.toUtf8();
        q.processFile(name.constData());
        q.getRoot().replaceKey("/Mervin_Measurements",
                               stream ? q.newStream(blob) : QPDFObjectHandle::newString(blob));
        const QByteArray copyName = copy.toUtf8();
        QPDFWriter writer(q, copyName.constData());
        writer.write();
    }
    QFile::remove(path);
    QFile::rename(copy, path);
}

// Build a valid PDF whose pages have the given MediaBox widths (all 792 tall).
// Distinct widths make each page identifiable after a copy, which is the only
// way to assert that pages came out in the right ORDER - makePdf's pages are all
// identical, so a reversed or sorted implementation passes a count-only check.
void makePdfSized(const QString &path, const QList<int> &widths)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper dh(q);
    for (int w : widths) {
        QPDFObjectHandle box = QPDFObjectHandle::newArray();
        box.appendItem(QPDFObjectHandle::newInteger(0));
        box.appendItem(QPDFObjectHandle::newInteger(0));
        box.appendItem(QPDFObjectHandle::newInteger(w));
        box.appendItem(QPDFObjectHandle::newInteger(792));
        QPDFObjectHandle page = QPDFObjectHandle::newDictionary();
        page.replaceKey("/Type", QPDFObjectHandle::newName("/Page"));
        page.replaceKey("/MediaBox", box);
        dh.addPage(QPDFPageObjectHelper(q.makeIndirectObject(page)), false);
    }
    const QByteArray outputName = path.toUtf8();
    QPDFWriter writer(q, outputName.constData());
    writer.write();
}

// Build a valid PDF with `n` blank Letter pages.
void makePdf(const QString &path, int n)
{
    makePdfSized(path, QList<int>(n, 612));
}

// The MediaBox widths of `path`, in page order - the fingerprint makePdfSized
// leaves behind.
QList<int> widthsOf(const QString &path)
{
    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(path, QString(), &err, nullptr);
    if (!doc)
        return {};
    QList<int> out;
    for (int i = 0; i < doc->pageCount(); ++i)
        out << qRound(doc->pageSize(i).width());
    return out;
}

// Removes /Length from the Encrypt dictionary of a file qpdf encrypted, as some
// writers do. The PDF specification makes it optional unless V is 2 or 3. Blanking
// the entry with spaces keeps every cross-reference offset valid.
bool omitKeyLength(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    QByteArray data = file.readAll();
    const QByteArray key("/Standard /Length ");
    const qsizetype at = data.indexOf(key);
    if (at < 0)
        return false;
    qsizetype end = at + key.size();
    while (end < data.size() && std::isdigit(static_cast<unsigned char>(data.at(end))))
        ++end;
    const qsizetype from = at + qsizetype(sizeof("/Standard ") - 1);
    data.replace(from, end - from, QByteArray(end - from, ' '));
    return file.seek(0) && file.write(data) == data.size() && file.flush();
}

// Writes a one-page PDF whose encryption `setUp` chooses on the writer, as another
// program would write it. Password bytes go to qpdf unchanged.
void makeEncrypted(const QString &path, const std::function<void(QPDFWriter &)> &setUp)
{
    QPDF q;
    q.emptyPDF();
    QPDFPageDocumentHelper(q).addPage(QPDFPageObjectHelper(q.makeIndirectObject(
        QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 612 792] /Resources << >> >>"))), false);
    const QByteArray outputName = path.toUtf8();
    QPDFWriter writer(q, outputName.constData());
    setUp(writer);
    writer.write();
}

// Opens `path` with qpdf and the exact password bytes, for checks of what a writer stored.
std::unique_ptr<QPDF> openWithBytes(const QString &path, const QByteArray &password)
{
    auto q = std::make_unique<QPDF>();
    q->processFile(path.toUtf8().constData(), password.constData());
    return q;
}

// Replaces the crypt filter names in a qpdf-written V4 or V5 file with `methods`, as other
// programs choose them. Key derivation ignores these names, so the passwords still open
// the file. The optional /AuthEvent /DocOpen entry makes room, so no offset moves.
void retargetCryptFilters(const QString &path, const QByteArray &methods)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QByteArray bytes = file.readAll();
    file.close();
    const QByteArray authEvent(" /AuthEvent /DocOpen");
    const QByteArray filters(" /StmF /StdCF /StrF /StdCF");
    QCOMPARE(bytes.count(authEvent), 1);
    QCOMPARE(bytes.count(filters), 1);
    const qsizetype room = authEvent.size() + filters.size();
    QVERIFY(methods.size() + 1 <= room);
    bytes.replace(authEvent, QByteArray());
    bytes.replace(filters, ' ' + methods + QByteArray(room - methods.size() - 1, ' '));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QCOMPARE(file.write(bytes), bytes.size());
}

} // namespace

class TstQpdf : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void init();

    void pageCountRoundTrip();
    void jpegDecodersCoexist();
    void encryptAes256ThenReadInfo();
    void decryptRemovesEncryption();
    void userPasswordRequiredToOpen();
    void permissionsRoundTrip();
    void legacyEncryptionStoresPdfDocPasswords_data();
    void legacyEncryptionStoresPdfDocPasswords();
    void legacyEncryptionRefusesOtherCharacters_data();
    void legacyEncryptionRefusesOtherCharacters();
    void aes256KeepsUnicodePasswords();
    void pdfDocKeyedSourcesOpenWithTheirPassword();
    void utf8KeyedSourcesStillOpen();
    void emptyOwnerPasswordKeepsRestrictions_data();
    void emptyOwnerPasswordKeepsRestrictions();
    void limitedPermissionsAreReported_data();
    void limitedPermissionsAreReported();
    void rc4RevisionsAreReportedAsRc4_data();
    void rc4RevisionsAreReportedAsRc4();
    void cryptFilterNamesTheAlgorithm_data();
    void cryptFilterNamesTheAlgorithm();
    void deletePagesReducesCount();
    void deleteLeavesOutDeletedContent_data();
    void deleteLeavesOutDeletedContent();
    void deleteKeepsEncryptionWithoutDeletedContent();
    void copiesKeepFormFields();
    void copiesLeaveOutFieldsOfOtherPages_data();
    void copiesLeaveOutFieldsOfOtherPages();
    void copiesKeepAFieldThatCopiedPagesShare();
    void copiesLeaveOutResourcesOfOtherPages_data();
    void copiesLeaveOutResourcesOfOtherPages();
    void copiesLeaveOutCommentsOfOtherPages_data();
    void copiesLeaveOutCommentsOfOtherPages();
    void copiesPointButtonsAtRenamedFields();
    void copiesScopeDocumentWideFormActions_data();
    void copiesScopeDocumentWideFormActions();
    void mergeNamesInputWithBadMeasurements_data();
    void mergeNamesInputWithBadMeasurements();
    void deleteNamesSourceWithBadMeasurements_data();
    void deleteNamesSourceWithBadMeasurements();
    void mergeConcatenates();
    void mergeHonoursPerFileRanges();
    void mergeRepeatsAPage();
    void mergeEmptyPagesMeansAll();
    void mergeUsesPerFilePassword();
    void mergeReportsFailedIndex();
    void mergeRejectsOutOfRangeIndex();
    void destinationConsentDistinguishesContentsAndErrors();
    void delayedMergeRequiresFreshConsent_data();
    void delayedMergeRequiresFreshConsent();
    void delayedSecurityRequiresFreshConsent_data();
    void delayedSecurityRequiresFreshConsent();
    void probeDistinguishesLockedFromUnreadable();
    void singleSourceCopiesKeepItsEncryption_data();
    void singleSourceCopiesKeepItsEncryption();
    void singleSourceCopiesKeepOwnerRestrictions();
    void mergeOfSeveralFilesIsNotEncrypted();
    void splitWritesOnePerPage();
    void rotateKeepsCount();
    void rotateAndDeleteKeepEncryptionWithoutKeyLength_data();
    void rotateAndDeleteKeepEncryptionWithoutKeyLength();
    void openEncryptedNeedsPassword();

private:
    QTemporaryDir dir_;
    QString in(const QString &name) const { return dir_.filePath(name); }
};

void TstQpdf::initTestCase()
{
    // The page-count error uses a %n plural, whose English forms come from the
    // English catalog.
    mervin::i18n::apply(QStringLiteral("en"));
}

void TstQpdf::init()
{
    QVERIFY(dir_.isValid());
}

/// Distinguishes absent output, exact contents and failed checks without trusting metadata.
void TstQpdf::destinationConsentDistinguishesContentsAndErrors()
{
    QTemporaryDir folder;
    const QString path = folder.filePath("output.pdf");
    QString error;
    const auto absent = mervin::DestinationConsent::read(path, &error);
    QVERIFY(absent);
    QVERIFY(!absent->exists);
    QFile output(path);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("first contents"), qint64(14));
    output.close();
    const auto first = mervin::DestinationConsent::read(path, &error);
    QVERIFY(first && first->exists);
    QVERIFY(*first != *absent);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("other contents"), qint64(14));
    output.close();
    const auto other = mervin::DestinationConsent::read(path, &error);
    QVERIFY(other);
    QVERIFY(*other != *first);
    QVERIFY(output.open(QIODevice::WriteOnly));
    QCOMPARE(output.write("first contents"), qint64(14));
    output.close();
    QCOMPARE(mervin::DestinationConsent::read(path, &error), first);
    QVERIFY(!mervin::DestinationConsent::read(folder.path(), &error));
    QVERIFY(!error.isEmpty());
#ifdef Q_OS_UNIX
    const QString dangling = folder.filePath("dangling.pdf");
    QVERIFY(QFile::link(folder.filePath("missing-target.pdf"), dangling));
    error.clear();
    QVERIFY(!mervin::DestinationConsent::read(dangling, &error));
    QVERIFY(!error.isEmpty());
#endif
}

/// Covers an output initially absent and an existing output already approved for replacement.
void TstQpdf::delayedMergeRequiresFreshConsent_data()
{
    QTest::addColumn<bool>("existing");
    QTest::newRow("absent output") << false;
    QTest::newRow("confirmed output") << true;
}

/// Preserves a completed winner when an older accepted request starts writing later.
void TstQpdf::delayedMergeRequiresFreshConsent()
{
    QFETCH(bool, existing);
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf");
    const QString output = folder.filePath("output.pdf");
    makePdf(source, 3);
    if (existing)
        makePdf(output, 3);
    const auto approved = mervin::DestinationConsent::read(output);
    QVERIFY(approved);
    QCOMPARE(approved->exists, existing);
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{source, {0}, {}}}, output),
             PageOps::Status::Ok);
    QFile winnerFile(output);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    const QByteArray winner = winnerFile.readAll();
    winnerFile.close();
    int validations = 0;
    const QList<PageOps::MergeInput> later{{source, {0, 1}, {}}};
    QCOMPARE(PageOps::merge(later, output, nullptr, nullptr, [&] {
        ++validations;
        return mervin::DestinationConsent::read(output) == approved;
    }), PageOps::Status::Failed);
    QCOMPARE(validations, 1);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    QCOMPARE(winnerFile.readAll(), winner);
    winnerFile.close();
    const auto confirmed = mervin::DestinationConsent::read(output);
    QVERIFY(confirmed);
    QCOMPARE(PageOps::merge(later, output, nullptr, nullptr, [&] {
        ++validations;
        return mervin::DestinationConsent::read(output) == confirmed;
    }), PageOps::Status::Ok);
    QCOMPARE(validations, 2);
    QCOMPARE(PageOps::pageCount(output), 2);
}

/// Covers both Security writers with absent and previously approved destinations.
void TstQpdf::delayedSecurityRequiresFreshConsent_data()
{
    QTest::addColumn<bool>("encrypt");
    QTest::addColumn<bool>("existing");
    QTest::newRow("encrypt absent") << true << false;
    QTest::newRow("encrypt existing") << true << true;
    QTest::newRow("decrypt absent") << false << false;
    QTest::newRow("decrypt existing") << false << true;
}

/// Both Qpdf writers preserve the winner until a new request approves its exact bytes.
void TstQpdf::delayedSecurityRequiresFreshConsent()
{
    QFETCH(bool, encrypt);
    QFETCH(bool, existing);
    QTemporaryDir folder;
    const QString source = folder.filePath("source.pdf");
    const QString encrypted = folder.filePath("encrypted.pdf");
    const QString output = folder.filePath("output.pdf");
    const QString password = QStringLiteral("source password");
    makePdf(source, 3);
    QpdfService service;
    QCOMPARE(service.encrypt(source, encrypted, {}, password, {}, QpdfService::Algorithm::AES256, {}),
             QpdfService::Status::Ok);
    if (existing)
        makePdf(output, 3);
    const auto approved = mervin::DestinationConsent::read(output);
    QVERIFY(approved);
    QCOMPARE(approved->exists, existing);
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{source, {0}, {}}}, output),
             PageOps::Status::Ok);
    QFile winnerFile(output);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    const QByteArray winner = winnerFile.readAll();
    winnerFile.close();
    int validations = 0;
    auto expected = approved;
    const auto validate = [&] {
        ++validations;
        return mervin::DestinationConsent::read(output) == expected;
    };
    const auto write = [&] {
        return encrypt
            ? service.encrypt(source, output, {}, password, {}, QpdfService::Algorithm::AES256,
                              {}, nullptr, validate)
            : service.decrypt(encrypted, output, password, nullptr, validate);
    };
    QCOMPARE(write(), QpdfService::Status::Failed);
    QCOMPARE(validations, 1);
    QVERIFY(winnerFile.open(QIODevice::ReadOnly));
    QCOMPARE(winnerFile.readAll(), winner);
    winnerFile.close();
    expected = mervin::DestinationConsent::read(output);
    QVERIFY(expected);
    QCOMPARE(write(), QpdfService::Status::Ok);
    QCOMPARE(validations, 2);
    QpdfService::Info info;
    QCOMPARE(service.readInfo(output, password, info), QpdfService::Status::Ok);
    QCOMPARE(info.encrypted, encrypt);
    QCOMPARE(PageOps::pageCount(output, password), 3);
}

void TstQpdf::pageCountRoundTrip()
{
    const QString p = in(QStringLiteral("count.pdf"));
    makePdf(p, 4);
    QCOMPARE(PageOps::pageCount(p), 4);
}

void TstQpdf::jpegDecodersCoexist()
{
    // qpdf's libjpeg and MuPDF's bundled JPEG 10 have different ABIs. Windows
    // links both into the executable; on Linux the shared libqpdf and Qt's JPEG
    // plugin bind to MuPDF's copy if the executable exports it. Encode with Qt,
    // then decode with qpdf and MuPDF in one process to detect symbol sharing.
    QImage source(16, 16, QImage::Format_RGB888);
    const QColor left(200, 32, 24);
    const QColor right(24, 64, 200);
    for (int y = 0; y < source.height(); ++y) {
        for (int x = 0; x < source.width(); ++x)
            source.setPixelColor(x, y, x < 8 ? left : right);
    }
    QByteArray jpeg;
    QBuffer buffer(&jpeg);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(source.save(&buffer, "JPEG", 100));

    QPDF pdf;
    pdf.emptyPDF();
    auto image = pdf.newStream(std::string(jpeg.constData(), jpeg.size()));
    auto dictionary = image.getDict();
    dictionary.replaceKey("/Type", QPDFObjectHandle::newName("/XObject"));
    dictionary.replaceKey("/Subtype", QPDFObjectHandle::newName("/Image"));
    dictionary.replaceKey("/Width", QPDFObjectHandle::newInteger(16));
    dictionary.replaceKey("/Height", QPDFObjectHandle::newInteger(16));
    dictionary.replaceKey("/BitsPerComponent", QPDFObjectHandle::newInteger(8));
    dictionary.replaceKey("/ColorSpace", QPDFObjectHandle::newName("/DeviceRGB"));
    dictionary.replaceKey("/Filter", QPDFObjectHandle::newName("/DCTDecode"));

    const auto decoded = image.getStreamData(qpdf_dl_all);
    QCOMPARE(decoded->getSize(), size_t(16 * 16 * 3));
    const QImage qpdfPixels(decoded->getBuffer(), 16, 16, 16 * 3, QImage::Format_RGB888);
    for (const int x : {4, 12}) {
        const QColor expected = x < 8 ? left : right;
        const QColor actual = qpdfPixels.pixelColor(x, 8);
        QVERIFY(qAbs(actual.red() - expected.red()) <= 3);
        QVERIFY(qAbs(actual.green() - expected.green()) <= 3);
        QVERIFY(qAbs(actual.blue() - expected.blue()) <= 3);
    }

    auto images = QPDFObjectHandle::newDictionary();
    images.replaceKey("/Image", image);
    auto resources = QPDFObjectHandle::newDictionary();
    resources.replaceKey("/XObject", images);
    auto page = QPDFObjectHandle::parse("<< /Type /Page /MediaBox [0 0 16 16] >>");
    page.replaceKey("/Resources", resources);
    page.replaceKey("/Contents", pdf.newStream("q 16 0 0 16 0 0 cm /Image Do Q"));
    QPDFPageDocumentHelper(pdf).addPage(QPDFPageObjectHelper(pdf.makeIndirectObject(page)), false);
    const QString path = in(QStringLiteral("jpeg-codecs.pdf"));
    const QByteArray name = path.toUtf8();
    QPDFWriter writer(pdf, name.constData());
    writer.setStreamDataMode(qpdf_s_preserve);
    writer.setDecodeLevel(qpdf_dl_none);
    writer.write();

    mervin::RenderEngine engine;
    QString error;
    auto document = engine.openDocument(path, {}, &error);
    QVERIFY2(document, qPrintable(error));
    const QImage rendered = engine.renderPageImage(document.get(), 0, 1, 0);
    QCOMPARE(rendered.size(), source.size());
    for (const int x : {4, 12}) {
        const QColor expected = x < 8 ? left : right;
        const QColor actual = rendered.pixelColor(x, 8);
        QVERIFY(qAbs(actual.red() - expected.red()) <= 3);
        QVERIFY(qAbs(actual.green() - expected.green()) <= 3);
        QVERIFY(qAbs(actual.blue() - expected.blue()) <= 3);
    }
}

void TstQpdf::encryptAes256ThenReadInfo()
{
    const QString src = in(QStringLiteral("src.pdf"));
    const QString enc = in(QStringLiteral("enc.pdf"));
    makePdf(src, 2);

    QpdfService svc;
    QString err;
    const auto st = svc.encrypt(src, enc, QString(), QStringLiteral("open"), QStringLiteral("owner"),
                                QpdfService::Algorithm::AES256, {}, &err);
    QCOMPARE(st, QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QStringLiteral("open"), info, &err), QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QCOMPARE(info.algorithm, QStringLiteral("AES-256"));
    QCOMPARE(info.keyLengthBits, 256);
}

void TstQpdf::decryptRemovesEncryption()
{
    const QString src = in(QStringLiteral("d_src.pdf"));
    const QString enc = in(QStringLiteral("d_enc.pdf"));
    const QString dec = in(QStringLiteral("d_dec.pdf"));
    makePdf(src, 2);

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("pw"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);
    QVERIFY(svc.decrypt(enc, dec, QStringLiteral("pw"), nullptr) == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(dec, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(!info.encrypted);
}

void TstQpdf::userPasswordRequiredToOpen()
{
    const QString src = in(QStringLiteral("u_src.pdf"));
    const QString enc = in(QStringLiteral("u_enc.pdf"));
    makePdf(src, 1);

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QString(), info, nullptr), QpdfService::Status::NeedsPassword);
    QCOMPARE(svc.readInfo(enc, QStringLiteral("secret"), info, nullptr), QpdfService::Status::Ok);
}

void TstQpdf::permissionsRoundTrip()
{
    const QString src = in(QStringLiteral("p_src.pdf"));
    const QString enc = in(QStringLiteral("p_enc.pdf"));
    makePdf(src, 1);

    QpdfService::Permissions perms; // owner-only encryption: no user password
    perms.canPrint = false;
    perms.canCopy = false;
    perms.canModify = false;

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QString(), QStringLiteral("owner"),
                        QpdfService::Algorithm::AES256, perms, nullptr)
            == QpdfService::Status::Ok);

    QpdfService::Info info;
    QCOMPARE(svc.readInfo(enc, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QVERIFY(!info.permissions.canPrint);
    QVERIFY(!info.permissions.canCopy);
    QVERIFY(!info.permissions.canModify);
    QVERIFY(info.permissions.canAnnotate);
    // Mervin's writer allows printing fully or not at all, and assembly with the rest.
    QVERIFY(!info.lowResolutionPrintOnly);
    QVERIFY(!info.pageAssemblyOnly);
}

void TstQpdf::legacyEncryptionStoresPdfDocPasswords_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::newRow("AES-128") << QpdfService::Algorithm::AES128;
    QTest::newRow("RC4-128") << QpdfService::Algorithm::RC4_128;
}

/// R4 keys use PDFDocEncoding, so MuPDF and qpdf both open the result with the typed text.
void TstQpdf::legacyEncryptionStoresPdfDocPasswords()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    const QString src = in(QStringLiteral("l_src.pdf"));
    const QString enc = in(QStringLiteral("l_enc.pdf"));
    makePdf(src, 2);
    const QString password = QStringLiteral("Müller");
    QString err;
    QCOMPARE(QpdfService().encrypt(src, enc, {}, password, {}, algorithm, {}, &err),
             QpdfService::Status::Ok);

    mervin::RenderEngine engine;
    bool needsPassword = false;
    auto doc = engine.openDocument(enc, password, &err, &needsPassword);
    QVERIFY2(doc, qPrintable(err));
    QCOMPARE(doc->pageCount(), 2);
    int count = 0;
    QCOMPARE(PageOps::probe(enc, &count, password, &err), PageOps::Status::Ok);
    QCOMPARE(count, 2);
    // The stored key is the PDFDocEncoding of the text, as other readers expect.
    QVERIFY(openWithBytes(enc, QByteArray("M\xFCller"))->userPasswordMatched());
}

void TstQpdf::legacyEncryptionRefusesOtherCharacters_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::addColumn<QString>("user");
    QTest::addColumn<QString>("owner");
    QTest::newRow("AES-128 open password")
        << QpdfService::Algorithm::AES128 << QStringLiteral("密码") << QString();
    QTest::newRow("RC4-128 owner password")
        << QpdfService::Algorithm::RC4_128 << QStringLiteral("open") << QStringLiteral("密码");
}

/// PDFDocEncoding has no Chinese characters, so legacy encryption writes nothing.
void TstQpdf::legacyEncryptionRefusesOtherCharacters()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QFETCH(QString, user);
    QFETCH(QString, owner);
    const QString src = in(QStringLiteral("x_src.pdf"));
    const QString enc = in(QStringLiteral("x_enc.pdf"));
    makePdf(src, 1);
    QString err;
    QCOMPARE(QpdfService().encrypt(src, enc, {}, user, owner, algorithm, {}, &err),
             QpdfService::Status::Failed);
    QCOMPARE(err, QpdfService::tr("A password contains characters that AES-128 and RC4-128 "
                                  "cannot use. Choose AES-256."));
    // Not even a temporary file.
    QCOMPARE(QDir(dir_.path()).entryList({QStringLiteral("x_enc*")}, QDir::Files | QDir::Hidden),
             QStringList());
}

/// AES-256 stores UTF-8, which MuPDF and qpdf both accept for any character.
void TstQpdf::aes256KeepsUnicodePasswords()
{
    const QString src = in(QStringLiteral("u8_src.pdf"));
    const QString enc = in(QStringLiteral("u8_enc.pdf"));
    makePdf(src, 1);
    const QString password = QStringLiteral("密码");
    QString err;
    QCOMPARE(QpdfService().encrypt(src, enc, {}, password, {}, QpdfService::Algorithm::AES256, {},
                                   &err),
             QpdfService::Status::Ok);
    mervin::RenderEngine engine;
    auto doc = engine.openDocument(enc, password, &err, nullptr);
    QVERIFY2(doc, qPrintable(err));
    QCOMPARE(PageOps::probe(enc, nullptr, password, &err), PageOps::Status::Ok);
    QVERIFY(openWithBytes(enc, password.toUtf8())->userPasswordMatched());
}

/// A third-party R4 file keyed with PDFDocEncoding opens with the typed text in every qpdf
/// reader, and its one-file copy keeps that key.
void TstQpdf::pdfDocKeyedSourcesOpenWithTheirPassword()
{
    const QString enc = in(QStringLiteral("pd_enc.pdf"));
    const QString out = in(QStringLiteral("pd_out.pdf"));
    makeEncrypted(enc, [](QPDFWriter &writer) {
        writer.setR4EncryptionParametersInsecure("caf\xE9", "caf\xE9", true, true, true, true, true,
                                                 true, qpdf_r3p_full, true, true);
    });
    const QString password = QStringLiteral("café");
    QString err;
    int count = 0;
    QCOMPARE(PageOps::probe(enc, &count, password, &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
    QpdfService::Info info;
    QCOMPARE(QpdfService().readInfo(enc, password, info, &err), QpdfService::Status::Ok);
    QCOMPARE(info.algorithm, QStringLiteral("AES-128"));
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{enc, {}, password}}, out, &err),
             PageOps::Status::Ok);
    // The copy of one file keeps its encryption, keyed with the same PDFDocEncoding bytes.
    QCOMPARE(PageOps::probe(out, &count, {}, &err), PageOps::Status::NeedsPassword);
    QCOMPARE(PageOps::probe(out, &count, password, &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
    QVERIFY(openWithBytes(out, QByteArray("caf\xE9"))->userPasswordMatched());
    // A wrong password still asks for the right one.
    QCOMPARE(PageOps::probe(enc, &count, QStringLiteral("cafe"), &err),
             PageOps::Status::NeedsPassword);
}

/// R4 files keyed with UTF-8, as earlier versions of Mervin wrote them, stay readable.
void TstQpdf::utf8KeyedSourcesStillOpen()
{
    const QString enc = in(QStringLiteral("u8k_enc.pdf"));
    const QByteArray utf8 = QStringLiteral("Müller").toUtf8();
    makeEncrypted(enc, [&utf8](QPDFWriter &writer) {
        writer.setR4EncryptionParametersInsecure(utf8.constData(), utf8.constData(), true, true,
                                                 true, true, true, true, qpdf_r3p_full, true, true);
    });
    QString err;
    int count = 0;
    QCOMPARE(PageOps::probe(enc, &count, QStringLiteral("Müller"), &err), PageOps::Status::Ok);
    QCOMPARE(count, 1);
}

void TstQpdf::emptyOwnerPasswordKeepsRestrictions_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::addColumn<QString>("user");
    QTest::addColumn<bool>("restricted");
    QTest::newRow("AES-128 restricted") << QpdfService::Algorithm::AES128 << QStringLiteral("x") << true;
    QTest::newRow("AES-256 restricted") << QpdfService::Algorithm::AES256 << QStringLiteral("x") << true;
    QTest::newRow("no open password, restricted")
        << QpdfService::Algorithm::AES256 << QString() << true;
    QTest::newRow("everything allowed") << QpdfService::Algorithm::AES128 << QStringLiteral("x") << false;
}

/// Restrictions get an owner password that the open password does not match.
void TstQpdf::emptyOwnerPasswordKeepsRestrictions()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QFETCH(QString, user);
    QFETCH(bool, restricted);
    const QString src = in(QStringLiteral("o_src.pdf"));
    const QString enc = in(QStringLiteral("o_enc.pdf"));
    const QString dec = in(QStringLiteral("o_dec.pdf"));
    makePdf(src, 1);
    QpdfService::Permissions perms;
    perms.canPrint = !restricted;
    QpdfService svc;
    QString err;
    QCOMPARE(svc.encrypt(src, enc, {}, user, {}, algorithm, perms, &err), QpdfService::Status::Ok);

    const auto q = openWithBytes(enc, user.toUtf8());
    QVERIFY(q->userPasswordMatched());
    // Owner rights ignore the flags, so restricted files must not grant them.
    // With everything allowed the open password stays the owner password, so other
    // programs still let its holder change the security settings.
    QCOMPARE(q->ownerPasswordMatched(), !restricted);
    QCOMPARE(q->allowPrintLowRes(), !restricted);

    // Strip Owner Restrictions needs only the open password.
    QCOMPARE(svc.decrypt(enc, dec, user, &err), QpdfService::Status::Ok);
    QpdfService::Info info;
    QCOMPARE(svc.readInfo(dec, {}, info, &err), QpdfService::Status::Ok);
    QVERIFY(!info.encrypted);
}

void TstQpdf::limitedPermissionsAreReported_data()
{
    QTest::addColumn<qpdf_r3_print_e>("print");
    QTest::addColumn<bool>("assemble");
    QTest::addColumn<bool>("modifyOther");
    QTest::addColumn<bool>("canPrint");
    QTest::addColumn<bool>("canModify");
    QTest::addColumn<bool>("lowResolution");
    QTest::addColumn<bool>("pageAssembly");
    QTest::newRow("low resolution and assembly only")
        << qpdf_r3p_low << true << false << false << false << true << true;
    QTest::newRow("everything") << qpdf_r3p_full << true << true << true << true << false << false;
    QTest::newRow("nothing") << qpdf_r3p_none << false << false << false << false << false << false;
    // Bit 4 alone already permits assembly in the PDF format.
    QTest::newRow("modify without bit 11")
        << qpdf_r3p_none << false << true << false << true << false << false;
}

/// Printing and modifying read as allowed only when every kind is allowed.
void TstQpdf::limitedPermissionsAreReported()
{
    QFETCH(qpdf_r3_print_e, print);
    QFETCH(bool, assemble);
    QFETCH(bool, modifyOther);
    QFETCH(bool, canPrint);
    QFETCH(bool, canModify);
    QFETCH(bool, lowResolution);
    QFETCH(bool, pageAssembly);
    const QString enc = in(QStringLiteral("lp_enc.pdf"));
    makeEncrypted(enc, [&](QPDFWriter &writer) {
        writer.setR4EncryptionParametersInsecure("", "owner", true, false, assemble, false, false,
                                                 modifyOther, print, true, true);
    });
    QpdfService::Info info;
    QString err;
    QCOMPARE(QpdfService().readInfo(enc, {}, info, &err), QpdfService::Status::Ok);
    QCOMPARE(info.permissions.canPrint, canPrint);
    QCOMPARE(info.permissions.canModify, canModify);
    QCOMPARE(info.lowResolutionPrintOnly, lowResolution);
    QCOMPARE(info.pageAssemblyOnly, pageAssembly);
}

void TstQpdf::rc4RevisionsAreReportedAsRc4_data()
{
    QTest::addColumn<int>("revision");
    QTest::addColumn<int>("bits");
    QTest::newRow("R2") << 2 << 40;
    QTest::newRow("R3") << 3 << 128;
}

/// qpdf reports no method for V1 and V2 files, which always use RC4.
void TstQpdf::rc4RevisionsAreReportedAsRc4()
{
    QFETCH(int, revision);
    QFETCH(int, bits);
    const QString enc = in(QStringLiteral("rc4_enc.pdf"));
    makeEncrypted(enc, [revision](QPDFWriter &writer) {
        if (revision == 2)
            writer.setR2EncryptionParametersInsecure("open", "owner", true, true, true, true);
        else
            writer.setR3EncryptionParametersInsecure("open", "owner", true, true, true, true, true,
                                                     true, qpdf_r3p_full);
    });
    QpdfService::Info info;
    QString err;
    QCOMPARE(QpdfService().readInfo(enc, QStringLiteral("open"), info, &err),
             QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QCOMPARE(info.revision, revision);
    QCOMPARE(info.algorithm, QStringLiteral("RC4"));
    QCOMPARE(info.keyLengthBits, bits);
}

void TstQpdf::cryptFilterNamesTheAlgorithm_data()
{
    QTest::addColumn<bool>("aes256");
    QTest::addColumn<QByteArray>("methods");
    QTest::addColumn<QString>("algorithm");
    QTest::addColumn<int>("bits");
    // Acrobat encrypts only attachments this way. Streams and strings stay plain.
    QTest::newRow("attachments only")
        << true << QByteArray("/StmF /Identity /StrF /Identity /EFF /StdCF")
        << QStringLiteral("AES-256") << 256;
    // The stream filter names the algorithm when attachments use another.
    QTest::newRow("streams before attachments")
        << false << QByteArray("/StmF /StdCF /StrF /StdCF /EFF /Identity")
        << QStringLiteral("AES-128") << 128;
}

/// V4 and V5 files name the first crypt filter in use, in stream, string, attachment order.
void TstQpdf::cryptFilterNamesTheAlgorithm()
{
    QFETCH(bool, aes256);
    QFETCH(QByteArray, methods);
    QFETCH(QString, algorithm);
    QFETCH(int, bits);
    const QString enc = in(QStringLiteral("cf_enc.pdf"));
    makeEncrypted(enc, [aes256](QPDFWriter &writer) {
        writer.setObjectStreamMode(qpdf_o_disable);
        if (aes256) {
            writer.setR6EncryptionParameters("open", "owner", true, true, true, true, true, true,
                                             qpdf_r3p_full, true);
        } else {
            writer.setR4EncryptionParametersInsecure("open", "owner", true, true, true, true, true,
                                                     true, qpdf_r3p_full, true, true);
        }
    });
    retargetCryptFilters(enc, methods);
    QpdfService::Info info;
    QString err;
    QCOMPARE(QpdfService().readInfo(enc, QStringLiteral("open"), info, &err),
             QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QCOMPARE(info.algorithm, algorithm);
    QCOMPARE(info.keyLengthBits, bits);
}

void TstQpdf::deletePagesReducesCount()
{
    const QString src = in(QStringLiteral("del_src.pdf"));
    const QString out = in(QStringLiteral("del_out.pdf"));
    makePdf(src, 5);
    QCOMPARE(PageOps::deletePages(src, out, {1, 3}), PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 3);
}

/// One row per way a document can refer to the page that Delete Pages removes.
void TstQpdf::deleteLeavesOutDeletedContent_data()
{
    QTest::addColumn<int>("reference");
    QTest::newRow("bookmark") << int(Reference::Outline);
    QTest::newRow("named bookmark") << int(Reference::NamedOutline);
    QTest::newRow("nested bookmarks") << int(Reference::NestedOutline);
    QTest::newRow("link") << int(Reference::Link);
    QTest::newRow("reply") << int(Reference::Reply);
    QTest::newRow("field") << int(Reference::Field);
    QTest::newRow("calculated parent field") << int(Reference::CalculatedField);
    QTest::newRow("child field") << int(Reference::ChildField);
    QTest::newRow("shared field") << int(Reference::SharedField);
    QTest::newRow("xfa") << int(Reference::Xfa);
    QTest::newRow("structure element") << int(Reference::Structure);
    QTest::newRow("nested structure") << int(Reference::NestedStructure);
    QTest::newRow("tagged widget") << int(Reference::TaggedWidget);
    QTest::newRow("reset button") << int(Reference::ResetButton);
    QTest::newRow("shared resources") << int(Reference::SharedResources);
    QTest::newRow("inherited resources") << int(Reference::InheritedResources);
    QTest::newRow("own resources") << int(Reference::OwnResources);
    QTest::newRow("direct bookmark") << int(Reference::DirectOutline);
    QTest::newRow("closed bookmarks") << int(Reference::ClosedOutline);
    QTest::newRow("widget on two pages") << int(Reference::SharedWidget);
    QTest::newRow("field without widget") << int(Reference::WidgetlessField);
    QTest::newRow("reply in a form") << int(Reference::ReplyInForm);
}

/// Checks that no object of a Delete Pages copy holds the deleted page's content, or what
/// only refers to that page, while the rest of the document stays.
void TstQpdf::deleteLeavesOutDeletedContent()
{
    QFETCH(int, reference);
    const auto ref = Reference(reference);
    const QString src = in(QStringLiteral("ref_src_%1.pdf").arg(reference));
    const QString out = in(QStringLiteral("ref_out_%1.pdf").arg(reference));
    makeReferencedPdf(src, ref);
    QVERIFY(pdfContains(src, kSecret)); // the scan finds what it looks for
    QCOMPARE(PageOps::deletePages(src, out, {1}), PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 2);
    QVERIFY(!pdfContains(out, kSecret));
    QVERIFY(pdfContains(out, "page one"));
    QVERIFY(pdfContains(out, "page three"));

    QPDF q;
    const QByteArray name = out.toUtf8();
    q.processFile(name.constData());
    QPDFObjectHandle root = q.getRoot();
    std::vector<QPDFObjectHandle> pages = q.getAllPages();
    QPDFObjectHandle form = root.getKey("/AcroForm");
    QPDFObjectHandle fields = form.isDictionary() ? form.getKey("/Fields") : QPDFObjectHandle::newArray();
    QPDFObjectHandle outlines = root.getKey("/Outlines");
    QPDFObjectHandle tree = root.getKey("/StructTreeRoot");
    const auto parentKeys = [&tree, &q] {
        QList<long long> keys;
        for (const auto &entry : QPDFNumberTreeObjectHelper(tree.getKey("/ParentTree"), q).getAsMap())
            keys << entry.first;
        return keys;
    };
    const auto documentKids = [&tree] {
        return tree.getKey("/K").getKey("/K").getArrayNItems();
    };
    switch (ref) {
    case Reference::Outline:
    case Reference::NamedOutline:
        QCOMPARE(outlines.getKey("/Count").getIntValue(), 1LL);
        QCOMPARE(outlines.getKey("/First").getObjGen(), outlines.getKey("/Last").getObjGen());
        QVERIFY(!outlines.getKey("/First").hasKey("/Next"));
        if (ref == Reference::NamedOutline) {
            // The names of page 1 stay.
            QCOMPARE(QPDFNameTreeObjectHelper(root.getKey("/Names").getKey("/Dests"), q).getAsMap().size(),
                     size_t(1));
            QVERIFY(QPDFNameTreeObjectHelper(root.getKey("/Names").getKey("/Dests"), q).hasName("pageone"));
            QVERIFY(root.getKey("/Dests").getKeys() == std::set<std::string>({"/pageone"}));
        }
        break;
    case Reference::DirectOutline: {
        QCOMPARE(outlines.getKey("/Count").getIntValue(), 2LL);
        QPDFObjectHandle first = outlines.getKey("/First");
        QCOMPARE(QString::fromStdString(first.getKey("/Title").getUTF8Value()), QStringLiteral("Direct one"));
        QCOMPARE(first.getKey("/Next").getObjGen(), outlines.getKey("/Last").getObjGen());
        QCOMPARE(QString::fromStdString(outlines.getKey("/Last").getKey("/Title").getUTF8Value()),
                 QStringLiteral("Page three"));
        break;
    }
    case Reference::ClosedOutline: {
        // Closed items count their remaining children as negative numbers.
        QCOMPARE(outlines.getKey("/Count").getIntValue(), 2LL);
        QPDFObjectHandle chapter = outlines.getKey("/First");
        QPDFObjectHandle section = outlines.getKey("/Last");
        QCOMPARE(chapter.getKey("/Count").getIntValue(), -1LL);
        QCOMPARE(QString::fromStdString(chapter.getKey("/First").getKey("/Title").getUTF8Value()),
                 QStringLiteral("Kept child"));
        QVERIFY(!section.hasKey("/First") && !section.hasKey("/Count"));
        break;
    }
    case Reference::SharedWidget: {
        // Page 1 keeps the widget, and the field keeps it too.
        QCOMPARE(pages.at(0).getKey("/Annots").getArrayNItems(), 1);
        QPDFObjectHandle twice = pages.at(0).getKey("/Annots").getArrayItem(0);
        QVERIFY(twice.isDictionary());
        QCOMPARE(fields.getArrayNItems(), 1);
        QCOMPARE(fields.getArrayItem(0).getObjGen(), twice.getObjGen());
        QVERIFY(pdfContains(out, "kept twice"));
        break;
    }
    case Reference::WidgetlessField:
        QCOMPARE(fields.getArrayNItems(), 1);
        QCOMPARE(QString::fromStdString(fields.getArrayItem(0).getKey("/T").getUTF8Value()),
                 QStringLiteral("data"));
        QVERIFY(pdfContains(out, "kept data"));
        break;
    case Reference::NestedOutline: {
        // The chapter stays for its child, the section loses its only child.
        QCOMPARE(outlines.getKey("/Count").getIntValue(), 3LL);
        QPDFObjectHandle chapter = outlines.getKey("/First");
        QPDFObjectHandle section = outlines.getKey("/Last");
        QCOMPARE(QString::fromStdString(chapter.getKey("/First").getKey("/Title").getUTF8Value()),
                 QStringLiteral("Kept child"));
        QCOMPARE(QString::fromStdString(section.getKey("/Title").getUTF8Value()), QStringLiteral("Section"));
        QVERIFY(!section.hasKey("/First") && !section.hasKey("/Last") && !section.hasKey("/Count"));
        QCOMPARE(section.getKey("/Prev").getObjGen(), chapter.getObjGen());
        break;
    }
    case Reference::Link:
        QCOMPARE(pages.at(0).getKey("/Annots").getArrayNItems(), 1);
        break;
    case Reference::Reply:
    case Reference::ReplyInForm:
        // The reply and its popup stay, without the note they answered.
        QCOMPARE(replyProblem(out, 0), QString());
        break;
    case Reference::Field:
    case Reference::TaggedWidget:
        QCOMPARE(fields.getArrayNItems(), 0);
        break;
    case Reference::CalculatedField:
        QCOMPARE(fields.getArrayNItems(), 1);
        QCOMPARE(form.getKey("/CO").getArrayNItems(), 1);
        QCOMPARE(form.getKey("/CO").getArrayItem(0).getObjGen(), fields.getArrayItem(0).getObjGen());
        break;
    case Reference::ChildField: {
        QCOMPARE(fields.getArrayNItems(), 1);
        QPDFObjectHandle kids = fields.getArrayItem(0).getKey("/Kids");
        QCOMPARE(kids.getArrayNItems(), 1);
        QCOMPARE(QString::fromStdString(kids.getArrayItem(0).getKey("/T").getUTF8Value()),
                 QStringLiteral("street"));
        QVERIFY(pdfContains(out, "kept street"));
        break;
    }
    case Reference::SharedField: {
        // The field and its page 1 widget stay, without the widget of page 2.
        QCOMPARE(fields.getArrayNItems(), 1);
        QPDFObjectHandle kids = fields.getArrayItem(0).getKey("/Kids");
        QCOMPARE(kids.getArrayNItems(), 1);
        QCOMPARE(kids.getArrayItem(0).getKey("/P").getObjGen(), pages.at(0).getObjGen());
        QVERIFY(pdfContains(out, "shared value"));
        break;
    }
    case Reference::Xfa:
        QCOMPARE(fields.getArrayNItems(), 1);
        QVERIFY(!form.hasKey("/XFA"));
        break;
    case Reference::Structure:
        QCOMPARE(documentKids(), 2);
        QCOMPARE(parentKeys(), QList<long long>({0, 2}));
        break;
    case Reference::NestedStructure: {
        // The section goes with its figure; the paragraph keeps its page 3 part.
        QCOMPARE(documentKids(), 3);
        QPDFObjectHandle across = tree.getKey("/K").getKey("/K").getArrayItem(1);
        QCOMPARE(across.getKey("/K").getArrayNItems(), 1);
        QCOMPARE(across.getKey("/K").getArrayItem(0).getKey("/Pg").getObjGen(),
                 pages.at(1).getObjGen());
        QCOMPARE(parentKeys(), QList<long long>({0, 2}));
        QVERIFY(QPDFNameTreeObjectHelper(tree.getKey("/IDTree"), q).getAsMap().empty());
        break;
    }
    case Reference::ResetButton: {
        // The button and the field it names stay, without the child field of page 2.
        QCOMPARE(fields.getArrayNItems(), 2);
        QCOMPARE(fields.getArrayItem(0).getKey("/Kids").getArrayNItems(), 1);
        QCOMPARE(fields.getArrayItem(1).getKey("/A").getKey("/Fields").getArrayItem(0).getObjGen(),
                 fields.getArrayItem(0).getObjGen());
        break;
    }
    case Reference::SharedResources:
    case Reference::InheritedResources:
    case Reference::OwnResources: {
        // Page 1 keeps its own image, without the images of the other pages.
        QPDFObjectHandle images =
            QPDFPageObjectHelper(pages.at(0)).getAttribute("/Resources", false).getKey("/XObject");
        QVERIFY(images.hasKey("/Im1"));
        QVERIFY(!images.hasKey("/Im2"));
        QVERIFY(pdfContains(out, "IMAGETHREE"));
        break;
    }
    case Reference::ChildFieldNoForm:
    case Reference::ResetByName:
    case Reference::SubmitButtons:
        break;
    }
    if (ref == Reference::TaggedWidget) {
        QCOMPARE(documentKids(), 2);
        QCOMPARE(parentKeys(), QList<long long>({0, 2}));
    }
}

/// Checks that an encrypted source gives an encrypted copy without the deleted content.
void TstQpdf::deleteKeepsEncryptionWithoutDeletedContent()
{
    const QString src = in(QStringLiteral("ref_enc_src.pdf"));
    const QString enc = in(QStringLiteral("ref_enc.pdf"));
    const QString out = in(QStringLiteral("ref_enc_out.pdf"));
    makeReferencedPdf(src, Reference::SharedField);
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
    QVERIFY(pdfContains(enc, kSecret, QStringLiteral("secret")));
    QCOMPARE(PageOps::deletePages(enc, out, {1}, QStringLiteral("secret")), PageOps::Status::Ok);
    QpdfService::Info info;
    QCOMPARE(svc.readInfo(out, QString(), info, nullptr), QpdfService::Status::NeedsPassword);
    QCOMPARE(svc.readInfo(out, QStringLiteral("secret"), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(info.encrypted);
    QVERIFY(!pdfContains(out, kSecret, QStringLiteral("secret")));
    QVERIFY(pdfContains(out, "shared value", QStringLiteral("secret")));
}

/// Checks that Merge, Extract, a repeated page and Split write forms that can be filled.
void TstQpdf::copiesKeepFormFields()
{
    const QString a = in(QStringLiteral("form_a.pdf"));
    const QString b = in(QStringLiteral("form_b.pdf"));
    const QString enc = in(QStringLiteral("form_enc.pdf"));
    const QString out = in(QStringLiteral("form_out.pdf"));
    // The same field names in both files, so the merge has to tell them apart.
    makeFormPdf(a, {100, 200}, "field");
    makeFormPdf(b, {300, 400}, "field");

    QCOMPARE(PageOps::merge({a, b}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 200, 300, 400}));
    QCOMPARE(formProblem(out, 4), QString());

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{a, {1}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({200}));
    QCOMPARE(formProblem(out, 1), QString());

    // A repeated page gets its own widget and field, so the copies fill in separately.
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{a, {0, 0}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 100}));
    QCOMPARE(formProblem(out, 2), QString());

    QpdfService svc;
    QCOMPARE(svc.encrypt(a, enc, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {}, QStringLiteral("secret")}, {b, {0}, {}}}, out),
             PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 200, 300}));
    QCOMPARE(formProblem(out, 3), QString());

    QStringList parts;
    QCOMPARE(PageOps::split(a, dir_.path(), QStringLiteral("form-part"), QString(), &parts),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 2);
    for (const QString &part : parts)
        QCOMPARE(formProblem(part, 1), QString());

    // Extract and Split of an encrypted form keep its encryption and its fields.
    QpdfService::Info info;
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{enc, {1}, QStringLiteral("secret")}}, out),
             PageOps::Status::Ok);
    QCOMPARE(svc.readInfo(out, QString(), info, nullptr), QpdfService::Status::NeedsPassword);
    QCOMPARE(formProblem(out, 1, QStringLiteral("secret")), QString());
    parts.clear();
    QCOMPARE(PageOps::split(enc, dir_.path(), QStringLiteral("form-enc-part"), QStringLiteral("secret"),
                            &parts),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 2);
    for (const QString &part : parts) {
        QCOMPARE(svc.readInfo(part, QString(), info, nullptr), QpdfService::Status::NeedsPassword);
        QCOMPARE(formProblem(part, 1, QStringLiteral("secret")), QString());
    }
}

/// Fixtures whose page 1 widget shares a field tree with a field or widget of page 2. The
/// widget count is that of page 1, or 0 for a source without /AcroForm.
void TstQpdf::copiesLeaveOutFieldsOfOtherPages_data()
{
    QTest::addColumn<int>("reference");
    QTest::addColumn<QString>("kept");
    QTest::addColumn<int>("widgets");
    QTest::newRow("child field") << int(Reference::ChildField) << QStringLiteral("kept street") << 1;
    QTest::newRow("shared field") << int(Reference::SharedField) << QStringLiteral("shared value") << 1;
    QTest::newRow("reset button") << int(Reference::ResetButton) << QStringLiteral("kept one") << 2;
    QTest::newRow("no form") << int(Reference::ChildFieldNoForm) << QStringLiteral("kept street") << 0;
}

/// Checks that Extract, Merge and Split copy no form data of pages they leave out, and keep
/// a value that a parent field shares with a widget they copy.
void TstQpdf::copiesLeaveOutFieldsOfOtherPages()
{
    QFETCH(int, reference);
    QFETCH(QString, kept);
    QFETCH(int, widgets);
    const QString src = in(QStringLiteral("other_src_%1.pdf").arg(reference));
    const QString out = in(QStringLiteral("other_out_%1.pdf").arg(reference));
    makeReferencedPdf(src, Reference(reference));
    const std::string keptText = kept.toStdString();

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0}, {}}}, out), PageOps::Status::Ok);
    QVERIFY(!pdfContains(out, kSecret));
    QVERIFY(pdfContains(out, keptText));
    if (widgets > 0)
        QCOMPARE(formProblem(out, widgets), QString());
    if (Reference(reference) == Reference::ResetButton) {
        // The copied button resets the copied field, not a copy that no page shows.
        QPDF q;
        const QByteArray name = out.toUtf8();
        q.processFile(name.constData());
        QPDFObjectHandle fields = q.getRoot().getKey("/AcroForm").getKey("/Fields");
        QCOMPARE(fields.getArrayNItems(), 2);
        QCOMPARE(fields.getArrayItem(1).getKey("/A").getKey("/Fields").getArrayItem(0).getObjGen(),
                 fields.getArrayItem(0).getObjGen());
    }

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {2, 0}, {}}}, out), PageOps::Status::Ok);
    QVERIFY(!pdfContains(out, kSecret));
    QVERIFY(pdfContains(out, keptText));

    QStringList parts;
    QCOMPARE(PageOps::split(src, dir_.path(), QStringLiteral("other-%1").arg(reference), QString(),
                            &parts),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 3);
    QVERIFY(!pdfContains(parts.at(0), kSecret));
    QVERIFY(pdfContains(parts.at(0), keptText));
    QVERIFY(!pdfContains(parts.at(2), kSecret));
    QVERIFY(!pdfContains(parts.at(2), keptText));
}

/// Checks that a field shown on two copied pages stays one field, while a repeated page gets
/// a field of its own.
void TstQpdf::copiesKeepAFieldThatCopiedPagesShare()
{
    const QString src = in(QStringLiteral("shared_src.pdf"));
    const QString out = in(QStringLiteral("shared_out.pdf"));
    makeReferencedPdf(src, Reference::SharedField);
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0, 1}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(fieldPages(out), QStringList({QStringLiteral("shared 1 2")}));
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0, 1, 0}, {}}}, out),
             PageOps::Status::Ok);
    QCOMPARE(fieldPages(out),
             QStringList({QStringLiteral("shared 1 2"), QStringLiteral("shared+1 3")}));
    mervin::RenderEngine engine;
    QString err;
    auto doc = engine.openDocument(out, QString(), &err, nullptr);
    QVERIFY2(doc, qPrintable(err));
    QVERIFY(doc->hasForm());
}

/// Sources whose pages list each other's images: through one resources dictionary, directly
/// or through /Pages, or through dictionaries of their own that list the same images.
void TstQpdf::copiesLeaveOutResourcesOfOtherPages_data()
{
    QTest::addColumn<int>("reference");
    QTest::newRow("shared resources") << int(Reference::SharedResources);
    QTest::newRow("inherited resources") << int(Reference::InheritedResources);
    QTest::newRow("own resources") << int(Reference::OwnResources);
}

/// Checks that Extract, Merge and Split copy only the images that the copied pages draw.
void TstQpdf::copiesLeaveOutResourcesOfOtherPages()
{
    QFETCH(int, reference);
    const QString src = in(QStringLiteral("res_src_%1.pdf").arg(reference));
    const QString out = in(QStringLiteral("res_out_%1.pdf").arg(reference));
    makeReferencedPdf(src, Reference(reference));

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0}, {}}}, out), PageOps::Status::Ok);
    QVERIFY(!pdfContains(out, kSecret));
    QVERIFY(pdfContains(out, "IMAGEONE"));
    QVERIFY(!pdfContains(out, "IMAGETHREE"));

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {2, 0}, {}}, {src, {0}, {}}}, out),
             PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 3);
    QVERIFY(!pdfContains(out, kSecret));
    QVERIFY(pdfContains(out, "IMAGEONE"));
    QVERIFY(pdfContains(out, "IMAGETHREE"));

    QStringList parts;
    QCOMPARE(PageOps::split(src, dir_.path(), QStringLiteral("res-%1").arg(reference), QString(),
                            &parts),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 3);
    QVERIFY(!pdfContains(parts.at(0), kSecret));
    QVERIFY(!pdfContains(parts.at(0), "IMAGETHREE"));
    QVERIFY(pdfContains(parts.at(1), kSecret));
    QVERIFY(!pdfContains(parts.at(2), kSecret));
    QVERIFY(pdfContains(parts.at(2), "IMAGETHREE"));
}

/// A reply to a note on page 2, in a file without and with a form.
void TstQpdf::copiesLeaveOutCommentsOfOtherPages_data()
{
    QTest::addColumn<int>("reference");
    QTest::newRow("reply") << int(Reference::Reply);
    QTest::newRow("reply in a form") << int(Reference::ReplyInForm);
}

/// Checks that Extract, Merge and Split leave out the note that a copied reply answers when
/// they leave out its page, as Delete Pages does, and keep the reply on its page.
void TstQpdf::copiesLeaveOutCommentsOfOtherPages()
{
    QFETCH(int, reference);
    const QString src = in(QStringLiteral("comments_src_%1.pdf").arg(reference));
    const QString other = in(QStringLiteral("comments_other_%1.pdf").arg(reference));
    const QString out = in(QStringLiteral("comments_out_%1.pdf").arg(reference));
    makeReferencedPdf(src, Reference(reference));
    makePdf(other, 1);

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0}, {}}}, out), PageOps::Status::Ok);
    QVERIFY(!pdfContains(out, kSecret));
    QCOMPARE(replyProblem(out, 0), QString());

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {2, 0}, {}}}, out), PageOps::Status::Ok);
    QVERIFY(!pdfContains(out, kSecret));
    QCOMPARE(replyProblem(out, 1), QString());

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{other, {}, {}}, {src, {0}, {}}}, out),
             PageOps::Status::Ok);
    QVERIFY(!pdfContains(out, kSecret));
    QCOMPARE(replyProblem(out, 1), QString());

    // With both pages, the reply still answers the note, the one that page 2 lists, and the
    // note keeps the popup that page 2 lists.
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0, 1}, {}}}, out), PageOps::Status::Ok);
    QVERIFY(pdfContains(out, kSecret));
    QCOMPARE(replyProblem(out, 0), QStringLiteral("the reply still answers a note"));
    {
        QPDF q;
        const QByteArray name = out.toUtf8();
        q.processFile(name.constData());
        std::vector<QPDFObjectHandle> pages = q.getAllPages();
        const auto listed = [&pages](size_t page, const std::string &contents) {
            for (QPDFObjectHandle annot : pages.at(page).getKey("/Annots").aitems())
                if (annot.getKey("/Contents").isString()
                    && annot.getKey("/Contents").getUTF8Value() == contents)
                    return annot;
            return QPDFObjectHandle::newNull();
        };
        QPDFObjectHandle reply = listed(0, "Reply");
        QPDFObjectHandle note = listed(1, kSecret + " note");
        QVERIFY(reply.isDictionary() && note.isDictionary());
        QCOMPARE(reply.getKey("/IRT").getObjGen(), note.getObjGen());
        std::set<QPDFObjGen> onPage2;
        for (QPDFObjectHandle annot : pages.at(1).getKey("/Annots").aitems())
            onPage2.insert(annot.getObjGen());
        QVERIFY(onPage2.count(note.getKey("/Popup").getObjGen()));
    }

    QStringList parts;
    QCOMPARE(PageOps::split(src, dir_.path(), QStringLiteral("comments-%1").arg(reference), QString(),
                            &parts),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 3);
    QVERIFY(!pdfContains(parts.at(0), kSecret));
    QCOMPARE(replyProblem(parts.at(0), 0), QString());
    QVERIFY(pdfContains(parts.at(1), kSecret));
}

/// Checks that a copied button that names fields by text names the copies after a repeated
/// page or a merged copy of the same form renames them.
void TstQpdf::copiesPointButtonsAtRenamedFields()
{
    const QString src = in(QStringLiteral("byname_src.pdf"));
    const QString again = in(QStringLiteral("byname_again.pdf"));
    const QString out = in(QStringLiteral("byname_out.pdf"));
    makeReferencedPdf(src, Reference::ResetByName);
    makeReferencedPdf(again, Reference::ResetByName);
    const QStringList renamed{QStringLiteral("name name"), QStringLiteral("name+1 name+1")};

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0, 0}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(fieldPages(out), QStringList({QStringLiteral("name 1"), QStringLiteral("reset 1"),
                                           QStringLiteral("name+1 2"), QStringLiteral("reset+1 2")}));
    QCOMPARE(resetTargets(out), renamed);

    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0}, {}}, {again, {0}, {}}}, out),
             PageOps::Status::Ok);
    QCOMPARE(resetTargets(out), renamed);

    // Without a rename, the names stay.
    QCOMPARE(PageOps::merge(QList<PageOps::MergeInput>{{src, {0}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(resetTargets(out), QStringList({QStringLiteral("name name")}));
}

/// Actions that act on every field, or every field but some, in a merge of two forms and in
/// a copy of one.
void TstQpdf::copiesScopeDocumentWideFormActions_data()
{
    const QString own =
        QStringLiteral("name other submit except none group exceptchild emptylist resetlater");
    QTest::addColumn<bool>("merged");
    QTest::addColumn<QString>("field");
    QTest::addColumn<QStringList>("keys");
    QTest::addColumn<QString>("scope");
    QTest::newRow("merge, submit all") << true << "submit" << QStringList{"/A"}
                                       << "SubmitForm: " + own;
    QTest::newRow("merge, then reset all") << true << "submit" << QStringList{"/A", "/Next"}
                                           << "ResetForm: " + own;
    QTest::newRow("merge, reset all on blur") << true << "name" << QStringList{"/AA", "/Bl"}
                                              << "ResetForm: " + own;
    QTest::newRow("merge, all but one") << true << "except" << QStringList{"/A"}
                                        << QStringLiteral("SubmitForm: other submit except none group "
                                                          "exceptchild emptylist resetlater");
    QTest::newRow("merge, all but a child") << true << "exceptchild" << QStringList{"/A"}
                                            << QStringLiteral("SubmitForm: name other submit except none "
                                                              "group.b exceptchild emptylist resetlater");
    QTest::newRow("merge, empty list") << true << "emptylist" << QStringList{"/A"}
                                       << "ResetForm: " + own;
    // The page 2 field that it lists is left out, so it would act on none, or on every field.
    QTest::newRow("merge, list of fields left out") << true << "resetlater" << QStringList{"/A"}
                                                    << QStringLiteral("no action");
    // An action left with no field goes, and the actions after it take its place, as
    // viewers such as PDF.js read an empty /Fields as every field.
    QTest::newRow("merge, all but all, then more") << true << "none" << QStringList{"/A"}
                                                   << QStringLiteral("SubmitForm: name");
    QTest::newRow("merge, all but all within a chain") << true << "except" << QStringList{"/A", "/Next"}
                                                       << QStringLiteral("SubmitForm: other");
    QTest::newRow("merge, all but all on focus") << true << "other" << QStringList{"/AA", "/Fo"}
                                                 << QStringLiteral("no action");
    QTest::newRow("one file, submit all") << false << "submit" << QStringList{"/A"}
                                          << QStringLiteral("SubmitForm: all");
    QTest::newRow("one file, all but one") << false << "except" << QStringList{"/A"}
                                           << QStringLiteral("SubmitForm: but name");
    QTest::newRow("one file, all but all") << false << "none" << QStringList{"/A"}
                                           << "ResetForm: but " + own;
    QTest::newRow("one file, all but all on focus") << false << "other" << QStringList{"/AA", "/Fo"}
                                                    << "ResetForm: but " + own;
    QTest::newRow("one file, empty list") << false << "emptylist" << QStringList{"/A"}
                                          << QStringLiteral("ResetForm: ");
    QTest::newRow("one file, list of fields left out") << false << "resetlater" << QStringList{"/A"}
                                                       << QStringLiteral("no action");
}

/// Checks that a merge limits a copied submit or reset action that acts on every field, or
/// every field but some, to the fields of its own file, and that a copy of one file keeps it.
void TstQpdf::copiesScopeDocumentWideFormActions()
{
    QFETCH(bool, merged);
    QFETCH(QString, field);
    QFETCH(QStringList, keys);
    QFETCH(QString, scope);
    const QString src = in(QStringLiteral("scope_src.pdf"));
    const QString other = in(QStringLiteral("scope_other.pdf"));
    const QString out = in(QStringLiteral("scope_out.pdf"));
    makeReferencedPdf(src, Reference::SubmitButtons);
    makeFormPdf(other, {300}, "field");
    QList<PageOps::MergeInput> inputs{{src, {0}, {}}};
    if (merged)
        inputs.append({other, {}, {}});
    QCOMPARE(PageOps::merge(inputs, out), PageOps::Status::Ok);
    QCOMPARE(actionScope(out, field.toStdString(), keys), scope);
    if (merged)
        QVERIFY(fieldPages(out).contains(QStringLiteral("field1 2")));
}

/// A string blob and a stream from a newer measurement format.
void TstQpdf::mergeNamesInputWithBadMeasurements_data()
{
    QTest::addColumn<bool>("stream");
    QTest::addColumn<QString>("blob");
    QTest::addColumn<QString>("message");
    QTest::newRow("string") << false << QStringLiteral("measurements")
                            << QStringLiteral("Invalid measurement metadata.");
    QTest::newRow("version 2") << true << QStringLiteral("{\"mervinMeasureVersion\":2}")
                               << QStringLiteral("Unsupported measurement metadata.");
}

/// Checks that a merge input whose measurements cannot be read is named by its index.
void TstQpdf::mergeNamesInputWithBadMeasurements()
{
    QFETCH(bool, stream);
    QFETCH(QString, blob);
    QFETCH(QString, message);
    const QString good = in(QStringLiteral("mb_good.pdf"));
    const QString bad = in(QStringLiteral("mb_bad.pdf"));
    const QString out = in(QStringLiteral("mb_out.pdf"));
    makePdf(good, 2);
    makePdf(bad, 2);
    setMeasurementBlob(bad, blob.toStdString(), stream);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{good, {}, {}}, {bad, {}, {}}}, out, &err, &failed),
             PageOps::Status::Failed);
    QCOMPARE(failed, 1);
    QCOMPARE(err, message);
    QVERIFY(!QFileInfo::exists(out));
}

/// The blobs of mergeNamesInputWithBadMeasurements.
void TstQpdf::deleteNamesSourceWithBadMeasurements_data()
{
    mergeNamesInputWithBadMeasurements_data();
}

/// Checks that Delete Pages names its source when it cannot read the measurements, and not
/// when the output cannot be written.
void TstQpdf::deleteNamesSourceWithBadMeasurements()
{
    QFETCH(bool, stream);
    QFETCH(QString, blob);
    QFETCH(QString, message);
    const QString good = in(QStringLiteral("db_good.pdf"));
    const QString bad = in(QStringLiteral("db_bad.pdf"));
    const QString out = in(QStringLiteral("db_out.pdf"));
    makePdf(good, 2);
    makePdf(bad, 2);
    setMeasurementBlob(bad, blob.toStdString(), stream);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::deletePages(bad, out, {0}, {}, &err, &failed), PageOps::Status::Failed);
    QCOMPARE(failed, 0);
    QCOMPARE(err, message);
    QVERIFY(!QFileInfo::exists(out));
    failed = 0;
    QCOMPARE(PageOps::deletePages(good, in(QStringLiteral("missing/db_out.pdf")), {0}, {}, &err,
                                  &failed),
             PageOps::Status::Failed);
    QCOMPARE(failed, -1);
}

void TstQpdf::mergeConcatenates()
{
    const QString a = in(QStringLiteral("m_a.pdf"));
    const QString b = in(QStringLiteral("m_b.pdf"));
    const QString out = in(QStringLiteral("m_out.pdf"));
    makePdfSized(a, {100, 200});
    makePdfSized(b, {300, 400, 500});
    QCOMPARE(PageOps::merge({a, b}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 200, 300, 400, 500}));
}

void TstQpdf::mergeHonoursPerFileRanges()
{
    const QString a = in(QStringLiteral("mr_a.pdf"));
    const QString b = in(QStringLiteral("mr_b.pdf"));
    const QString out = in(QStringLiteral("mr_out.pdf"));
    makePdfSized(a, {100, 200, 300});
    makePdfSized(b, {400, 500, 600});
    // b's pages 3 and 1, then a's page 2 - an order no sort produces by accident.
    QCOMPARE(PageOps::merge({{b, {2, 0}, {}}, {a, {1}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({600, 400, 200}));
}

void TstQpdf::mergeRepeatsAPage()
{
    const QString a = in(QStringLiteral("mrep_a.pdf"));
    const QString out = in(QStringLiteral("mrep_out.pdf"));
    makePdfSized(a, {100, 200, 300});
    // The same file as two inputs, and the same page twice within one input:
    // "cover page repeated at the back" is the case the dialog's Duplicate exists
    // for, and it must not be silently deduplicated.
    QCOMPARE(PageOps::merge({{a, {0, 0}, {}}, {a, {2}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 100, 300}));
}

void TstQpdf::mergeEmptyPagesMeansAll()
{
    const QString a = in(QStringLiteral("me_a.pdf"));
    const QString b = in(QStringLiteral("me_b.pdf"));
    const QString out = in(QStringLiteral("me_out.pdf"));
    makePdfSized(a, {100, 200});
    makePdfSized(b, {300});
    QCOMPARE(PageOps::merge({{a, {}, {}}, {b, {}, {}}}, out), PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({100, 200, 300}));
}

void TstQpdf::mergeUsesPerFilePassword()
{
    const QString src = in(QStringLiteral("mp_src.pdf"));
    const QString enc = in(QStringLiteral("mp_enc.pdf"));
    const QString plain = in(QStringLiteral("mp_plain.pdf"));
    const QString out = in(QStringLiteral("mp_out.pdf"));
    makePdfSized(src, {100, 200});
    makePdfSized(plain, {300});

    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    // No password: refused, and the offending input is named by index.
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{plain, {}, {}}, {enc, {}, {}}}, out, &err, &failed),
             PageOps::Status::NeedsPassword);
    QCOMPARE(failed, 1);

    // With it: merges, in order.
    QCOMPARE(PageOps::merge({{plain, {}, {}}, {enc, {1}, QStringLiteral("secret")}}, out, &err,
                            &failed),
             PageOps::Status::Ok);
    QCOMPARE(widthsOf(out), QList<int>({300, 200}));
    QCOMPARE(failed, -1);
}

void TstQpdf::mergeReportsFailedIndex()
{
    const QString a = in(QStringLiteral("mf_a.pdf"));
    const QString out = in(QStringLiteral("mf_out.pdf"));
    makePdf(a, 1);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{a, {}, {}}, {in(QStringLiteral("nope.pdf")), {}, {}}, {a, {}, {}}},
                            out, &err, &failed),
             PageOps::Status::Failed);
    QCOMPARE(failed, 1);
    QVERIFY(!err.isEmpty());
}

void TstQpdf::mergeRejectsOutOfRangeIndex()
{
    // An out-of-range index fails rather than being skipped. A page count that
    // went stale between the dialog's probe and this write would otherwise write a
    // shorter document than the dialog promised and still report success.
    const QString a = in(QStringLiteral("mo_a.pdf"));
    const QString out = in(QStringLiteral("mo_out.pdf"));
    makePdf(a, 2);
    QString err;
    int failed = -1;
    QCOMPARE(PageOps::merge({{a, {0, 9}, {}}}, out, &err, &failed), PageOps::Status::Failed);
    QCOMPARE(failed, 0);
    // Names the 1-based page, and the count reads as English, not "2 page(s)".
    QCOMPARE(err, QStringLiteral("It has 2 pages; page 10 does not exist."));

    const QString single = in(QStringLiteral("mo_single.pdf"));
    makePdf(single, 1);
    QCOMPARE(PageOps::merge({{single, {2}, {}}}, out, &err, &failed), PageOps::Status::Failed);
    QCOMPARE(err, QStringLiteral("It has 1 page; page 3 does not exist."));
}

void TstQpdf::probeDistinguishesLockedFromUnreadable()
{
    const QString src = in(QStringLiteral("pr_src.pdf"));
    const QString enc = in(QStringLiteral("pr_enc.pdf"));
    makePdf(src, 4);
    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    int n = -1;
    QCOMPARE(PageOps::probe(src, &n), PageOps::Status::Ok);
    QCOMPARE(n, 4);
    QCOMPARE(PageOps::probe(enc, &n), PageOps::Status::NeedsPassword);
    QCOMPARE(PageOps::probe(enc, &n, QStringLiteral("secret")), PageOps::Status::Ok);
    QCOMPARE(n, 4);
    QCOMPARE(PageOps::probe(in(QStringLiteral("absent.pdf")), &n), PageOps::Status::Failed);
}

/// Covers each algorithm Mervin writes, V4 and V5 files without the optional /Length,
/// and the revision 3 RC4 files of older writers.
void TstQpdf::singleSourceCopiesKeepItsEncryption_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::addColumn<bool>("withoutLength");
    QTest::addColumn<QString>("label");
    QTest::addColumn<int>("bits");
    QTest::addColumn<int>("revision");
    QTest::addColumn<QString>("password");
    // The PDFDocEncoding bytes the copies must be keyed with, when they differ from UTF-8.
    QTest::addColumn<QByteArray>("pdfDocKey");
    const QString secret = QStringLiteral("secret");
    QTest::newRow("AES-256")
        << QpdfService::Algorithm::AES256 << false << QStringLiteral("AES-256") << 256 << 6
        << secret << QByteArray();
    QTest::newRow("AES-128")
        << QpdfService::Algorithm::AES128 << false << QStringLiteral("AES-128") << 128 << 4
        << secret << QByteArray();
    // qpdf writes every V4 copy with AES, as Acrobat does.
    QTest::newRow("RC4-128")
        << QpdfService::Algorithm::RC4_128 << false << QStringLiteral("AES-128") << 128 << 4
        << secret << QByteArray();
    QTest::newRow("AES-128 without /Length")
        << QpdfService::Algorithm::AES128 << true << QStringLiteral("AES-128") << 128 << 4
        << secret << QByteArray();
    QTest::newRow("AES-256 without /Length")
        << QpdfService::Algorithm::AES256 << true << QStringLiteral("AES-256") << 256 << 6
        << secret << QByteArray();
    // Revision 3 keeps RC4. An empty label expects the source's own label.
    QTest::newRow("RC4-128 revision 3")
        << QpdfService::Algorithm::RC4_128 << false << QString() << 128 << 3 << secret
        << QByteArray();
    // AES-128 and RC4-128 key the source with the PDFDocEncoding form of the password,
    // and the copies keep that key.
    QTest::newRow("AES-128 with a Western European password")
        << QpdfService::Algorithm::AES128 << false << QStringLiteral("AES-128") << 128 << 4
        << QStringLiteral("Müller") << QByteArray("M\xFCller");
    QTest::newRow("RC4-128 with a Western European password")
        << QpdfService::Algorithm::RC4_128 << false << QStringLiteral("AES-128") << 128 << 4
        << QStringLiteral("Müller") << QByteArray("M\xFCller");
}

/// Extract, a merge of one file's ranges and Split open only with the source's passwords.
void TstQpdf::singleSourceCopiesKeepItsEncryption()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QFETCH(bool, withoutLength);
    QFETCH(QString, label);
    QFETCH(int, bits);
    QFETCH(int, revision);
    QFETCH(QString, password);
    QFETCH(QByteArray, pdfDocKey);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString src = files.filePath(QStringLiteral("src.pdf"));
    const QString enc = files.filePath(QStringLiteral("enc.pdf"));
    makePdfSized(src, {100, 200, 300});
    QpdfService svc;
    if (revision == 3) {
        // Mervin does not write revision 3, so qpdf builds this source directly.
        QPDF pdf;
        pdf.processFile(src.toUtf8().constData());
        const QByteArray name = enc.toUtf8();
        QPDFWriter writer(pdf, name.constData());
        const QByteArray user = password.toUtf8(); // ASCII, the same in every encoding
        writer.setR3EncryptionParametersInsecure(user.constData(), "owner", true, true, true, true,
                                                 true, true, qpdf_r3p_full);
        writer.write();
    } else {
        QCOMPARE(svc.encrypt(src, enc, QString(), password,
                             QStringLiteral("owner"), algorithm, {}, nullptr),
                 QpdfService::Status::Ok);
    }
    QString err;
    if (label.isEmpty()) {
        QpdfService::Info source;
        QCOMPARE(svc.readInfo(enc, password, source, &err),
                 QpdfService::Status::Ok);
        QCOMPARE(source.revision, revision);
        label = source.algorithm;
    }
    if (withoutLength) {
        QVERIFY(omitKeyLength(enc));
        int pages = 0;
        QCOMPARE(PageOps::probe(enc, &pages, password), PageOps::Status::Ok);
    }

    const QString extracted = files.filePath(QStringLiteral("extracted.pdf"));
    const QString ranges = files.filePath(QStringLiteral("ranges.pdf"));
    QCOMPARE(PageOps::merge({{enc, {1}, password}}, extracted, &err),
             PageOps::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {2}, password},
                             {enc, {0, 1}, password}},
                            ranges, &err),
             PageOps::Status::Ok);
    QStringList parts;
    QCOMPARE(PageOps::split(enc, files.path(), QStringLiteral("part"), password,
                            &parts, &err),
             PageOps::Status::Ok);
    QCOMPARE(parts.size(), 3);

    // Each output with the source page widths it should hold, in order.
    const QList<std::pair<QString, QList<int>>> outputs{
        {extracted, {200}}, {ranges, {300, 100, 200}}, {parts.at(0), {100}}, {parts.at(2), {300}}};
    for (const auto &[path, widths] : outputs) {
        const int count = int(widths.size());
        int pages = -1;
        QCOMPARE(PageOps::probe(path, &pages), PageOps::Status::NeedsPassword);
        QCOMPARE(PageOps::probe(path, &pages, QStringLiteral("owner")), PageOps::Status::Ok);
        QCOMPARE(PageOps::probe(path, &pages, password), PageOps::Status::Ok);
        QCOMPARE(pages, count);
        QpdfService::Info info;
        QCOMPARE(svc.readInfo(path, password, info, &err), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        QCOMPARE(info.algorithm, label);
        QCOMPARE(info.keyLengthBits, bits);
        QCOMPARE(info.revision, revision);
        // MuPDF, which shows the copy, reads it with the same password.
        mervin::RenderEngine engine;
        bool needsPassword = false;
        QVERIFY(!engine.openDocument(path, QString(), &err, &needsPassword));
        QVERIFY(needsPassword);
        const auto doc = engine.openDocument(path, password, &err, &needsPassword);
        QVERIFY2(doc, qPrintable(err));
        QList<int> shown;
        for (int i = 0; i < doc->pageCount(); ++i)
            shown << qRound(doc->pageSize(i).width());
        QCOMPARE(shown, widths);
        if (!pdfDocKey.isEmpty())
            QVERIFY(openWithBytes(path, pdfDocKey)->userPasswordMatched());
    }
}

/// A copy of a file with only an owner password keeps its permission flags.
void TstQpdf::singleSourceCopiesKeepOwnerRestrictions()
{
    const QString src = in(QStringLiteral("or_src.pdf"));
    const QString enc = in(QStringLiteral("or_enc.pdf"));
    const QString out = in(QStringLiteral("or_out.pdf"));
    makePdf(src, 2);
    QpdfService::Permissions perms;
    perms.canPrint = false;
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QString(), QStringLiteral("owner"),
                         QpdfService::Algorithm::AES256, perms, nullptr),
             QpdfService::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {1}, {}}, {enc, {0}, {}}}, out), PageOps::Status::Ok);
    QStringList parts;
    QCOMPARE(PageOps::split(enc, dir_.path(), QStringLiteral("or_part"), QString(), &parts),
             PageOps::Status::Ok);
    for (const QString &path : {out, parts.at(0)}) {
        QpdfService::Info info;
        QCOMPARE(svc.readInfo(path, QString(), info, nullptr), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        QVERIFY(!info.permissions.canPrint);
        QVERIFY(info.permissions.canCopy);
    }
}

/// A merge that reads several files writes no encryption, even when one input has it.
void TstQpdf::mergeOfSeveralFilesIsNotEncrypted()
{
    const QString src = in(QStringLiteral("mx_src.pdf"));
    const QString enc = in(QStringLiteral("mx_enc.pdf"));
    const QString plain = in(QStringLiteral("mx_plain.pdf"));
    const QString out = in(QStringLiteral("mx_out.pdf"));
    makePdf(src, 2);
    makePdf(plain, 1);
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                         QpdfService::Algorithm::AES256, {}, nullptr),
             QpdfService::Status::Ok);
    QCOMPARE(PageOps::merge({{enc, {}, QStringLiteral("secret")}, {plain, {}, {}}}, out),
             PageOps::Status::Ok);
    QpdfService::Info info;
    QCOMPARE(svc.readInfo(out, QString(), info, nullptr), QpdfService::Status::Ok);
    QVERIFY(!info.encrypted);
}

void TstQpdf::splitWritesOnePerPage()
{
    const QString src = in(QStringLiteral("s_src.pdf"));
    makePdf(src, 3);
    QStringList outs;
    QCOMPARE(PageOps::split(src, dir_.path(), QStringLiteral("part"), QString(), &outs),
             PageOps::Status::Ok);
    QCOMPARE(outs.size(), 3);
    for (const QString &f : outs)
        QCOMPARE(PageOps::pageCount(f), 1);
}

void TstQpdf::rotateKeepsCount()
{
    const QString src = in(QStringLiteral("r_src.pdf"));
    const QString out = in(QStringLiteral("r_out.pdf"));
    makePdf(src, 3);
    QCOMPARE(PageOps::rotatePages(src, out, {0, 2}, 90, true), PageOps::Status::Ok);
    QCOMPARE(PageOps::pageCount(out), 3);
}

/// Covers V4 files with AES and with RC4, which Mervin writes with /Length.
void TstQpdf::rotateAndDeleteKeepEncryptionWithoutKeyLength_data()
{
    QTest::addColumn<QpdfService::Algorithm>("algorithm");
    QTest::newRow("AES-128") << QpdfService::Algorithm::AES128;
    QTest::newRow("RC4-128") << QpdfService::Algorithm::RC4_128;
}

/// Rotate and Delete rewrite a V4 file without /Length, still encrypted with its passwords.
void TstQpdf::rotateAndDeleteKeepEncryptionWithoutKeyLength()
{
    QFETCH(QpdfService::Algorithm, algorithm);
    QTemporaryDir files;
    QVERIFY(files.isValid());
    const QString src = files.filePath(QStringLiteral("src.pdf"));
    const QString enc = files.filePath(QStringLiteral("enc.pdf"));
    const QString rotated = files.filePath(QStringLiteral("rotated.pdf"));
    const QString deleted = files.filePath(QStringLiteral("deleted.pdf"));
    makePdfSized(src, {100, 200, 300});
    QpdfService svc;
    QCOMPARE(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QStringLiteral("owner"),
                         algorithm, {}, nullptr),
             QpdfService::Status::Ok);
    QVERIFY(omitKeyLength(enc));

    QString err;
    QCOMPARE(PageOps::rotatePages(enc, rotated, {1}, 90, true, QStringLiteral("secret"), &err),
             PageOps::Status::Ok);
    QCOMPARE(PageOps::deletePages(enc, deleted, {1}, QStringLiteral("secret"), &err),
             PageOps::Status::Ok);

    // Each output with its page widths, as MuPDF shows them, and rotations, in order.
    struct Output
    {
        QString path;
        QList<int> widths;
        QList<int> rotations;
    };
    const QList<Output> outputs{{rotated, {100, 792, 300}, {0, 90, 0}},
                                {deleted, {100, 300}, {0, 0}}};
    for (const auto &[path, widths, rotations] : outputs) {
        int pages = -1;
        QCOMPARE(PageOps::probe(path, &pages), PageOps::Status::NeedsPassword);
        QCOMPARE(PageOps::probe(path, &pages, QStringLiteral("owner")), PageOps::Status::Ok);
        QCOMPARE(PageOps::probe(path, &pages, QStringLiteral("secret")), PageOps::Status::Ok);
        QCOMPARE(pages, int(widths.size()));
        QpdfService::Info info;
        QCOMPARE(svc.readInfo(path, QStringLiteral("secret"), info, &err), QpdfService::Status::Ok);
        QVERIFY(info.encrypted);
        // qpdf writes every V4 file with AES.
        QCOMPARE(info.algorithm, QStringLiteral("AES-128"));
        QCOMPARE(info.keyLengthBits, 128);
        QCOMPARE(info.revision, 4);
        mervin::RenderEngine engine;
        const auto doc = engine.openDocument(path, QStringLiteral("secret"), &err);
        QVERIFY2(doc, qPrintable(err));
        QList<int> shown;
        for (int i = 0; i < doc->pageCount(); ++i)
            shown << qRound(doc->pageSize(i).width());
        QCOMPARE(shown, widths);
        QPDF pdf;
        pdf.processFile(path.toUtf8().constData(), "secret");
        QList<int> turned;
        for (auto &page : QPDFPageDocumentHelper(pdf).getAllPages()) {
            // Not const: qpdf 11's QPDFObjectHandle accessors are not const.
            auto rotate = page.getAttribute("/Rotate", false);
            turned << (rotate.isInteger() ? int(rotate.getIntValue()) : 0);
        }
        QCOMPARE(turned, rotations);
    }
}

void TstQpdf::openEncryptedNeedsPassword()
{
    const QString src = in(QStringLiteral("e_src.pdf"));
    const QString enc = in(QStringLiteral("e_enc.pdf"));
    makePdf(src, 1);
    QpdfService svc;
    QVERIFY(svc.encrypt(src, enc, QString(), QStringLiteral("secret"), QString(),
                        QpdfService::Algorithm::AES256, {}, nullptr)
            == QpdfService::Status::Ok);

    mervin::RenderEngine engine;
    QString err;
    bool needsPw = false;

    // No password -> refused, flagged as needing a password.
    QVERIFY(engine.openDocument(enc, QString(), &err, &needsPw) == nullptr);
    QVERIFY(needsPw);

    // Wrong password -> still refused.
    QVERIFY(engine.openDocument(enc, QStringLiteral("nope"), &err, &needsPw) == nullptr);
    QVERIFY(needsPw);

    // Correct password -> opens.
    auto doc = engine.openDocument(enc, QStringLiteral("secret"), &err, &needsPw);
    QVERIFY2(doc != nullptr, qPrintable(err));
    QVERIFY(!needsPw);
    QCOMPARE(doc->pageCount(), 1);
}

QTEST_GUILESS_MAIN(TstQpdf)
#include "tst_qpdf.moc"
