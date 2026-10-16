#include "security/PageOps.h"
#include "io/JobContext.h"
#include "security/AtomicPdfWriter.h"
#include "security/MeasurementPages.h"
#include "security/QpdfSource.h"
#include <algorithm>
#include <stdexcept>

#include <qpdf/Constants.h>
#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFExc.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <QDir>

#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mervin {

namespace {

std::string u8(const QString &s)
{
    const QByteArray b = s.toUtf8();
    return std::string(b.constData(), static_cast<size_t>(b.size()));
}

PageOps::Status open(QPDF &q, const QString &path, const QString &password, QString *error)
{
    if (!job::reading(path)) {
        if (error)
            *error = job::notRunError();
        return PageOps::Status::Failed;
    }
    try {
        const std::string pw = u8(password);
        processReportedFile(q, path, password.isEmpty() ? nullptr : pw.c_str());
        return PageOps::Status::Ok;
    } catch (const QPDFExc &e) {
        if (e.getErrorCode() == qpdf_e_password) {
            // QpdfService's message, so translators see it once.
            if (error)
                *error = QpdfService::tr("A password is required to open this document.");
            return PageOps::Status::NeedsPassword;
        }
        if (error)
            *error = QString::fromUtf8(e.what());
        return PageOps::Status::Failed;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return PageOps::Status::Failed;
    }
}

using ObjectSet = std::set<QPDFObjGen>;

// How deep the cleanup below follows form fields, outline items and structure elements.
// qpdf's own form traversal stops at the same depth.
constexpr int kMaxDepth = 100;

/// Adds the indirect annotations that `page` lists in /Annots to `into`.
void addAnnotations(QPDFObjectHandle page, ObjectSet &into)
{
    QPDFObjectHandle annots = page.getKey("/Annots");
    if (!annots.isArray())
        return;
    for (QPDFObjectHandle annot : annots.aitems())
        if (annot.isIndirect())
            into.insert(annot.getObjGen());
}

/// Removes the form fields whose widgets a page operation does not keep. A field stays
/// when it, or a field or widget below it, is a widget in `kept`. With `dropped`, as for
/// Delete Pages, a field also stays when nothing below it is in `dropped`, so fields that
/// no page shows remain. A field that goes leaves its parent's /Kids, and its own subtree
/// goes with it.
class FieldPruner
{
public:
    FieldPruner(const ObjectSet &kept, const ObjectSet *dropped) : kept_(kept), dropped_(dropped) {}

    /// Prunes the fields below `field` and returns whether `field` stays.
    bool prune(QPDFObjectHandle field) { return visit(field, 0).stays; }

    /// Removes the entries of `fields`, an array such as /Fields or /CO, that do not stay.
    void pruneArray(QPDFObjectHandle fields)
    {
        if (!fields.isArray())
            return;
        for (int i = 0; i < fields.getArrayNItems();)
            if (prune(fields.getArrayItem(i)))
                ++i;
            else
                fields.eraseItem(i);
    }

    /// The fields that went. Their descendants that hold no widget of `dropped` are not
    /// listed, as nothing shows them.
    const ObjectSet &removed() const { return removed_; }

private:
    struct Verdict
    {
        bool kept = false;    // a widget in `kept` is at or below the field
        bool dropped = false; // a widget in `dropped` is at or below the field
        bool stays = true;
    };

    Verdict visit(QPDFObjectHandle field, int depth)
    {
        // qpdf ignores direct and non-dictionary entries in the field tree as well.
        if (!field.isIndirect() || !field.isDictionary())
            return {};
        const QPDFObjGen id = field.getObjGen();
        if (const auto it = seen_.find(id); it != seen_.end())
            return it->second; // a field reached twice, or a loop back to one in progress
        seen_[id] = {};
        Verdict verdict;
        verdict.kept = kept_.count(id) > 0;
        verdict.dropped = dropped_ && dropped_->count(id) > 0;
        QPDFObjectHandle kids = field.getKey("/Kids");
        if (kids.isArray() && depth < kMaxDepth) {
            for (int i = 0; i < kids.getArrayNItems();) {
                const Verdict kid = visit(kids.getArrayItem(i), depth + 1);
                verdict.kept |= kid.kept;
                verdict.dropped |= kid.dropped;
                if (kid.stays)
                    ++i;
                else
                    kids.eraseItem(i);
            }
        }
        verdict.stays = verdict.kept || (dropped_ && !verdict.dropped);
        if (!verdict.stays)
            removed_.insert(id);
        seen_[id] = verdict;
        return verdict;
    }

    const ObjectSet &kept_;
    const ObjectSet *dropped_;
    std::map<QPDFObjGen, Verdict> seen_;
    ObjectSet removed_;
};

/// The objects of a Delete Pages source, collected before the pages go.
struct Removal
{
    ObjectSet pages;           // the deleted pages
    ObjectSet annotations;     // the indirect annotations that only deleted pages list
    ObjectSet keptAnnotations; // the indirect annotations that kept pages list
    std::set<long long> parentKeys; // ParentTree keys that only those pages and annotations use
};

/// Whether outline `item` does nothing but go to one of `pages`. A named destination is
/// looked up through `names`.
bool leadsOnlyTo(QPDFObjectHandle item, QPDFOutlineDocumentHelper &names, const ObjectSet &pages)
{
    QPDFObjectHandle dest = item.getKey("/Dest");
    QPDFObjectHandle action = item.getKey("/A");
    if (action.isDictionary()) {
        if (!action.getKey("/S").isNameAndEquals("/GoTo") || action.hasKey("/Next"))
            return false;
        if (dest.isNull())
            dest = action.getKey("/D");
    }
    if (dest.isName() || dest.isString())
        dest = names.resolveNamedDest(dest);
    if (dest.isDictionary())
        dest = dest.getKey("/D");
    if (!dest.isArray() || dest.getArrayNItems() == 0)
        return false;
    QPDFObjectHandle page = dest.getArrayItem(0);
    return page.isIndirect() && pages.count(page.getObjGen()) > 0;
}

/// Adds to `doomed` the outline items below `parent` that only go to a deleted page and
/// keep no items below them. Returns whether any item below `parent` stays.
bool findDeadOutlines(QPDFObjectHandle parent, QPDFOutlineDocumentHelper &names,
                      const ObjectSet &pages, ObjectSet &seen, ObjectSet &doomed, int depth)
{
    bool stays = false;
    for (QPDFObjectHandle item = parent.getKey("/First");
         item.isIndirect() && item.isDictionary() && seen.insert(item.getObjGen()).second;
         item = item.getKey("/Next")) {
        const bool kids = depth < kMaxDepth
            ? findDeadOutlines(item, names, pages, seen, doomed, depth + 1)
            : item.hasKey("/First");
        if (!kids && leadsOnlyTo(item, names, pages))
            doomed.insert(item.getObjGen());
        else
            stays = true;
    }
    return stays;
}

/// Unlinks the doomed items below `parent`, links the rest again and recounts /Count
/// where anything below changed. Returns whether anything below `parent` changed.
bool unlinkOutlines(QPDFObjectHandle parent, const ObjectSet &doomed, ObjectSet &seen, int depth)
{
    std::vector<QPDFObjectHandle> kept;
    bool changed = false;
    for (QPDFObjectHandle item = parent.getKey("/First");
         item.isIndirect() && item.isDictionary() && seen.insert(item.getObjGen()).second;
         item = item.getKey("/Next")) {
        if (doomed.count(item.getObjGen())) {
            changed = true;
            continue;
        }
        if (depth < kMaxDepth && unlinkOutlines(item, doomed, seen, depth + 1))
            changed = true;
        kept.push_back(item);
    }
    if (!changed)
        return false;
    if (kept.empty()) {
        for (const char *key : {"/First", "/Last", "/Count"})
            parent.removeKey(key);
        return true;
    }
    parent.replaceKey("/First", kept.front());
    parent.replaceKey("/Last", kept.back());
    long long visible = 0; // the items shown below `parent` while it is open
    for (size_t i = 0; i < kept.size(); ++i) {
        if (i > 0)
            kept[i].replaceKey("/Prev", kept[i - 1]);
        else
            kept[i].removeKey("/Prev");
        if (i + 1 < kept.size())
            kept[i].replaceKey("/Next", kept[i + 1]);
        else
            kept[i].removeKey("/Next");
        QPDFObjectHandle count = kept[i].getKey("/Count");
        visible += 1 + (count.isInteger() ? std::max(0LL, count.getIntValue()) : 0);
    }
    // The outline root counts every open item. An item counts its open descendants,
    // negative while it is closed, and an item without /Count stays without it.
    QPDFObjectHandle count = parent.getKey("/Count");
    if (depth == 0 || (count.isInteger() && count.getIntValue() > 0))
        parent.replaceKey("/Count", QPDFObjectHandle::newInteger(visible));
    else if (count.isInteger() && count.getIntValue() < 0)
        parent.replaceKey("/Count", QPDFObjectHandle::newInteger(-visible));
    return true;
}

/// Removes the outline items that go only to a deleted page and have no items left below
/// them, as their titles can describe the page. A damaged outline stays as it was.
void pruneOutlines(QPDF &q, const ObjectSet &pages)
{
    QPDFObjectHandle outlines = q.getRoot().getKey("/Outlines");
    if (!outlines.isDictionary())
        return;
    ObjectSet doomed;
    try {
        QPDFOutlineDocumentHelper names(q);
        ObjectSet seen;
        findDeadOutlines(outlines, names, pages, seen, doomed, 0);
    } catch (const std::exception &) {
        return;
    }
    ObjectSet seen;
    if (!doomed.empty())
        unlinkOutlines(outlines, doomed, seen, 0);
}

/// Finds the structure elements that mark content only on deleted pages. read() only
/// looks, so a damaged structure tree can be left as it was; apply() then removes those
/// elements with their /ParentTree and /IDTree entries, and takes the deleted pages'
/// content items out of the elements that stay.
class StructurePruner
{
public:
    StructurePruner(QPDF &q, const Removal &removal) : q_(q), removal_(removal) {}

    void read(QPDFObjectHandle root)
    {
        forEachKid(root, QPDFObjectHandle::newNull(), [this](QPDFObjectHandle kid,
                                                             QPDFObjectHandle page) {
            classify(kid, page, 0);
        });
        QPDFObjectHandle parents = root.getKey("/ParentTree");
        if (parents.isDictionary())
            for (const auto &[key, value] : QPDFNumberTreeObjectHelper(parents, q_).getAsMap())
                if (removal_.parentKeys.count(key) || isDoomed(value))
                    parentKeys_.push_back(key);
        QPDFObjectHandle ids = root.getKey("/IDTree");
        if (ids.isDictionary())
            for (const auto &[key, value] : QPDFNameTreeObjectHelper(ids, q_).getAsMap())
                if (isDoomed(value))
                    ids_.push_back(key);
    }

    void apply(QPDFObjectHandle root)
    {
        if (!parentKeys_.empty()) {
            QPDFNumberTreeObjectHelper parents(root.getKey("/ParentTree"), q_);
            for (long long key : parentKeys_)
                parents.remove(key);
        }
        if (!ids_.empty()) {
            QPDFNameTreeObjectHelper ids(root.getKey("/IDTree"), q_);
            for (const std::string &key : ids_)
                ids.remove(key);
        }
        ObjectSet trimmed;
        trim(root, QPDFObjectHandle::newNull(), trimmed, 0);
        for (const QPDFObjGen &id : doomed_)
            q_.replaceObject(id, QPDFObjectHandle::newNull());
    }

private:
    enum Place { None = 0, Kept = 1, Deleted = 2 };

    // Calls `visit` with each item of `parent`'s /K and the page an MCID there is on.
    template <typename Visit>
    static void forEachKid(QPDFObjectHandle parent, QPDFObjectHandle page, Visit visit)
    {
        QPDFObjectHandle own = parent.getKey("/Pg");
        if (!own.isNull())
            page = own;
        QPDFObjectHandle kids = parent.getKey("/K");
        if (kids.isArray()) {
            for (QPDFObjectHandle kid : kids.aitems())
                visit(kid, page);
        } else if (!kids.isNull()) {
            visit(kids, page);
        }
    }

    // A dictionary in /K is an element unless it refers to marked content or an object.
    static bool isElement(QPDFObjectHandle kid)
    {
        return kid.isDictionary() && !kid.hasKey("/MCID") && !kid.hasKey("/Obj")
            && !kid.getKey("/Type").isNameAndEquals("/MCR")
            && !kid.getKey("/Type").isNameAndEquals("/OBJR");
    }

    // Kept for any page that is not a deleted one, so unplaced content stays.
    int placeOf(QPDFObjectHandle page) const
    {
        return page.isIndirect() && removal_.pages.count(page.getObjGen()) ? Deleted : Kept;
    }

    // Where content item `kid` (an MCID, a marked-content or an object reference) is.
    int placeOfContent(QPDFObjectHandle kid, QPDFObjectHandle page) const
    {
        if (kid.isInteger())
            return placeOf(page);
        QPDFObjectHandle own = kid.getKey("/Pg");
        if (!own.isNull())
            page = own;
        QPDFObjectHandle object = kid.getKey("/Obj");
        if (object.isIndirect()) {
            if (removal_.keptAnnotations.count(object.getObjGen()))
                return Kept;
            if (removal_.annotations.count(object.getObjGen()))
                return Deleted;
        }
        return placeOf(page);
    }

    // Where the element and its descendants have content. An element without content
    // items of its own counts as being on its page, if it has one.
    int classify(QPDFObjectHandle elem, QPDFObjectHandle page, int depth)
    {
        if (!isElement(elem))
            return None;
        if (depth > kMaxDepth)
            throw std::runtime_error("structure tree too deep");
        const bool indirect = elem.isIndirect();
        if (indirect) {
            if (const auto it = places_.find(elem.getObjGen()); it != places_.end())
                return it->second;
            places_[elem.getObjGen()] = None; // a loop back to it adds nothing
        }
        int place = None;
        bool content = false;
        QPDFObjectHandle own = elem.getKey("/Pg");
        forEachKid(elem, page, [&](QPDFObjectHandle kid, QPDFObjectHandle kidPage) {
            if (isElement(kid)) {
                place |= classify(kid, kidPage, depth + 1);
            } else if (kid.isInteger() || kid.isDictionary()) {
                content = true;
                place |= placeOfContent(kid, kidPage);
            }
        });
        if (!content) {
            QPDFObjectHandle at = own.isNull() ? page : own;
            if (at.isIndirect() && removal_.pages.count(at.getObjGen()))
                place |= Deleted;
        }
        if (indirect) {
            places_[elem.getObjGen()] = place;
            if (place == Deleted)
                doomed_.insert(elem.getObjGen());
        }
        return place;
    }

    bool isDoomed(QPDFObjectHandle value) const
    {
        return value.isIndirect() && doomed_.count(value.getObjGen());
    }

    // Removes doomed elements, and content on deleted pages, from the /K of `parent` and
    // of the elements below it that stay. The kids of `parent` are at `depth`, which is
    // the depth read() gave them.
    void trim(QPDFObjectHandle parent, QPDFObjectHandle page, ObjectSet &trimmed, int depth)
    {
        if (parent.isIndirect() && !trimmed.insert(parent.getObjGen()).second)
            return;
        QPDFObjectHandle own = parent.getKey("/Pg");
        if (!own.isNull())
            page = own;
        const auto gone = [&](QPDFObjectHandle kid) {
            if (isElement(kid)) {
                // read() went through the same elements at the same depth, so classify()
                // does not throw.
                if (kid.isIndirect() ? isDoomed(kid) : classify(kid, page, depth) == Deleted)
                    return true;
                if (depth < kMaxDepth)
                    trim(kid, page, trimmed, depth + 1);
                return false;
            }
            return (kid.isInteger() || kid.isDictionary())
                && placeOfContent(kid, page) == Deleted;
        };
        QPDFObjectHandle kids = parent.getKey("/K");
        if (kids.isArray()) {
            for (int i = 0; i < kids.getArrayNItems();)
                if (gone(kids.getArrayItem(i)))
                    kids.eraseItem(i);
                else
                    ++i;
        } else if (!kids.isNull() && gone(kids)) {
            parent.removeKey("/K");
        }
    }

    QPDF &q_;
    const Removal &removal_;
    std::map<QPDFObjGen, int> places_;
    ObjectSet doomed_; // indirect elements with content only on deleted pages
    std::vector<long long> parentKeys_;
    std::vector<std::string> ids_;
};

/// Removes the structure elements that mark content only on deleted pages, as their /Alt
/// or /ActualText can describe the page. A damaged structure tree stays as it was.
void pruneStructure(QPDF &q, const Removal &removal)
{
    QPDFObjectHandle root = q.getRoot().getKey("/StructTreeRoot");
    if (!root.isDictionary())
        return;
    StructurePruner pruner(q, removal);
    try {
        pruner.read(root);
    } catch (const std::exception &) {
        return;
    }
    pruner.apply(root);
}

/// Removes the form fields that only deleted pages show, from /Fields, /CO and their
/// parents' /Kids, and returns them. Removing a field also drops /XFA, whose data holds
/// every field's value and would no longer match the form.
ObjectSet pruneDeletedFields(QPDF &q, const Removal &removal)
{
    QPDFObjectHandle form = q.getRoot().getKey("/AcroForm");
    if (!form.isDictionary())
        return {};
    FieldPruner pruner(removal.keptAnnotations, &removal.annotations);
    pruner.pruneArray(form.getKey("/Fields"));
    pruner.pruneArray(form.getKey("/CO"));
    if (!pruner.removed().empty())
        form.removeKey("/XFA");
    return pruner.removed();
}

/// Leaves out of `q` what only the deleted pages of `pages` used, after
/// QPDFPageDocumentHelper::removePage took them out of the page tree, which is all it
/// does. The pages, the annotations that no kept page lists and the form fields that only
/// deleted pages show become null, so outlines, links, structure and other references
/// cannot keep their content. Outline items and structure elements that lead only to
/// deleted pages go as well.
void removeDeletedContent(QPDF &q, const std::vector<QPDFPageObjectHelper> &pages,
                          const std::set<int> &drop)
{
    Removal removal;
    std::set<long long> keptKeys; // ParentTree keys that kept pages or their annotations use
    for (int i = 0; i < static_cast<int>(pages.size()); ++i) {
        QPDFObjectHandle page = pages[static_cast<size_t>(i)].getObjectHandle();
        const bool deleted = drop.count(i) > 0;
        if (deleted)
            removal.pages.insert(page.getObjGen());
        addAnnotations(page, deleted ? removal.annotations : removal.keptAnnotations);
        std::set<long long> &keys = deleted ? removal.parentKeys : keptKeys;
        if (page.getKey("/StructParents").isInteger())
            keys.insert(page.getKey("/StructParents").getIntValue());
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (annots.isArray())
            for (QPDFObjectHandle annot : annots.aitems())
                if (annot.isDictionary() && annot.getKey("/StructParent").isInteger())
                    keys.insert(annot.getKey("/StructParent").getIntValue());
    }
    for (const QPDFObjGen &id : removal.keptAnnotations)
        removal.annotations.erase(id);
    for (long long key : keptKeys)
        removal.parentKeys.erase(key);
    pruneOutlines(q, removal.pages);
    pruneStructure(q, removal);
    const ObjectSet fields = pruneDeletedFields(q, removal);
    const ObjectSet *gone[] = {&removal.pages, &removal.annotations, &fields};
    for (const ObjectSet *objects : gone)
        for (const QPDFObjGen &id : *objects)
            q.replaceObject(id, QPDFObjectHandle::newNull());
}

/// Form helpers for the documents that pages are copied from, each made once.
class SourceForms
{
public:
    /// The helper for `pdf`, or null when it has no form or qpdf cannot read the form, in
    /// which case its pages are copied as addPage leaves them.
    QPDFAcroFormDocumentHelper *of(QPDF &pdf)
    {
        auto it = forms_.find(&pdf);
        if (it == forms_.end()) {
            std::unique_ptr<QPDFAcroFormDocumentHelper> form;
            try {
                // The constructor, as QPDFAcroFormDocumentHelper::get needs qpdf 12.3.
                form = std::make_unique<QPDFAcroFormDocumentHelper>(pdf);
                if (!form->hasAcroForm())
                    form.reset();
            } catch (const std::exception &) {
                form.reset();
            }
            it = forms_.emplace(&pdf, std::move(form)).first;
        }
        return it->second.get();
    }

private:
    std::map<QPDF *, std::unique_ptr<QPDFAcroFormDocumentHelper>> forms_;
};

/// Copies form fields along with the pages that one output gets from QPDFPageDocumentHelper::
/// addPage, which copies only the page and leaves its fields out of the output's /AcroForm.
/// As in qpdf's own page selection, each copied page gets its own annotations and fields,
/// and a field whose name the output already has is renamed, for example to "name+1", so
/// two copies of one page fill in separately.
class FormCopy
{
public:
    FormCopy(QPDF &out, SourceForms &sources) : out_(out), form_(out), sources_(sources) {}

    /// Copies the annotations and fields of `page` from `source` onto the page that addPage
    /// just added last to the output.
    void copied(QPDF &source, QPDFPageObjectHelper page)
    {
        QPDFAcroFormDocumentHelper *from = sources_.of(source);
        if (!from)
            return;
        QPDFObjectHandle copy = out_.getAllPages().back();
        QPDFObjectHandle plain = copy.getKey("/Annots");
        try {
            form_.fixCopiedAnnotations(copy, page.getObjectHandle(), *from);
        } catch (const std::exception &) {
            // qpdf before 12.4 can throw on damaged annotations. Like qpdf, keep the page
            // as addPage copied it.
            if (plain.isNull())
                copy.removeKey("/Annots");
            else
                copy.replaceKey("/Annots", plain);
            return;
        }
        // qpdf before 12.3 leaves /P on the page the annotations came from, which is the
        // first copy when a page is copied again.
        QPDFObjectHandle annots = copy.getKey("/Annots");
        if (annots.isArray())
            for (QPDFObjectHandle annot : annots.aitems())
                if (annot.isDictionary() && annot.hasKey("/P"))
                    annot.replaceKey("/P", copy);
    }

    /// Removes the copied fields that have no widget on an output page. A field tree that
    /// spans several pages comes along whole, and the output must not carry the values of
    /// pages it leaves out. A value on a parent that a copied widget shares stays.
    void finish()
    {
        ObjectSet widgets;
        for (QPDFObjectHandle page : out_.getAllPages())
            addAnnotations(page, widgets);
        FieldPruner pruner(widgets, nullptr);
        QPDFObjectHandle root = out_.getRoot();
        QPDFObjectHandle form = root.getKey("/AcroForm");
        if (form.isDictionary()) {
            QPDFObjectHandle fields = form.getKey("/Fields");
            pruner.pruneArray(fields);
            if (!fields.isArray() || fields.getArrayNItems() == 0)
                root.removeKey("/AcroForm");
        }
        // The widgets of a page whose form could not be copied reach their copied field
        // tree only through /Parent. Fields have no /Type, which keeps the climb out of
        // anything else that a damaged /Parent might name.
        for (QPDFObjectHandle page : out_.getAllPages()) {
            QPDFObjectHandle annots = page.getKey("/Annots");
            if (!annots.isArray())
                continue;
            for (QPDFObjectHandle top : annots.aitems()) {
                if (!top.isDictionary() || !top.getKey("/Subtype").isNameAndEquals("/Widget"))
                    continue;
                for (int depth = 0; depth < kMaxDepth; ++depth) {
                    QPDFObjectHandle parent = top.getKey("/Parent");
                    if (!parent.isDictionary() || parent.hasKey("/Type"))
                        break;
                    top = parent;
                }
                pruner.prune(top);
            }
        }
    }

private:
    QPDF &out_;
    QPDFAcroFormDocumentHelper form_;
    SourceForms &sources_;
};

} // namespace

int PageOps::pageCount(const QString &path, const QString &password)
{
    QPDF q;
    if (open(q, path, password, nullptr) != Status::Ok)
        return -1;
    try {
        return static_cast<int>(QPDFPageDocumentHelper(q).getAllPages().size());
    } catch (const std::exception &) {
        return -1;
    }
}

PageOps::Status PageOps::probe(const QString &path, int *count, const QString &password,
                               QString *error, bool *encrypted)
{
    QPDF q;
    const Status st = open(q, path, password, error);
    if (st != Status::Ok)
        return st;
    try {
        if (count)
            *count = static_cast<int>(QPDFPageDocumentHelper(q).getAllPages().size());
        if (encrypted)
            *encrypted = q.isEncrypted();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

namespace {

/// Sets the source's /Length to the key length qpdf opened it with, in memory only.
/// qpdf's writer takes the key length from /Length when it keeps or copies encryption,
/// and V4 and V5 files may omit it. Call it before writing an output encrypted like
/// `source`. Does nothing for an unencrypted source.
void setKeyLength(QPDF &source)
{
    if (!source.isEncrypted())
        return;
    source.getTrailer().getKey("/Encrypt").replaceKey(
        "/Length", QPDFObjectHandle::newInteger(
                       static_cast<long long>(source.getEncryptionKey().size()) * 8));
}

} // namespace

PageOps::Status PageOps::deletePages(const QString &inPath, const QString &outPath,
                                     const QList<int> &pages, const QString &password,
                                     QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        const std::set<int> drop(pages.begin(), pages.end());
        QList<int> kept;
        for (int index : drop)
            if (index < 0 || index >= int(all.size()))
                throw std::runtime_error(tr("Page does not exist.").toStdString());
        if (drop.size() == all.size())
            throw std::runtime_error(tr("Cannot delete every page.").toStdString());
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            if (drop.count(i))
                dh.removePage(all[static_cast<size_t>(i)]);
            else
                kept.append(i);
        removeDeletedContent(q, all, drop);
        MeasureDoc mapped;
        measurementPages::append(mapped, measurementPages::read(q), kept, 0);
        measurementPages::write(q, mapped);
        setKeyLength(q);
        AtomicPdfWriter output(q, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

PageOps::Status PageOps::rotatePages(const QString &inPath, const QString &outPath,
                                     const QList<int> &pages, int angle, bool relative,
                                     const QString &password, QString *error)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        auto data = measurementPages::read(q);
        if (angle % 90 != 0)
            throw std::runtime_error(
                tr("Rotation must be a multiple of 90 degrees.").toStdString());
        for (int idx : pages) {
            if (idx < 0 || idx >= int(all.size()))
                throw std::runtime_error(tr("Page does not exist.").toStdString());
            auto &page = all[idx];
            auto rotation = page.getAttribute("/Rotate", false);
            const int before = rotation.isInteger() ? int(rotation.getIntValue()) : 0;
            auto box = page.getCropBox().getArrayAsRectangle();
            double width = box.urx - box.llx, height = box.ury - box.lly;
            auto unit = page.getObjectHandle().getKey("/UserUnit");
            const double factor = unit.isNumber() ? unit.getNumericValue() : 1.0;
            width *= factor;
            height *= factor;
            if ((before % 180 + 180) % 180)
                std::swap(width, height);
            page.rotatePage(angle, relative);
            const int after = int(page.getAttribute("/Rotate", false).getIntValue());
            const int delta = ((after - before) % 360 + 360) % 360;
            const auto turn = [=](QPointF point) {
                switch (delta) {
                case 90: return QPointF(height - point.y(), point.x());
                case 180: return QPointF(width - point.x(), height - point.y());
                case 270: return QPointF(point.y(), width - point.x());
                default: return point;
                }
            };
            for (auto &mark : data.measurements)
                if (mark.page == idx) {
                    for (auto &point : mark.pts)
                        point = turn(point);
                    if (mark.hasLabelPos)
                        mark.labelPos = turn(mark.labelPos);
                }
            if (delta == 90 || delta == 270)
                for (auto &scale : data.pageScales)
                    if (scale.page == idx)
                        std::swap(scale.mmPerPointX, scale.mmPerPointY);
        }
        measurementPages::write(q, data);
        setKeyLength(q);
        AtomicPdfWriter output(q, outPath);
        output.write();
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

namespace {

/// Encrypts `output` like `source`, with the same passwords and permission flags.
/// Leaves it unencrypted when the source is.
void copyEncryption(AtomicPdfWriter &output, QPDF &source)
{
    if (!source.isEncrypted())
        return;
    setKeyLength(source);
    output.options().copyEncryptionParameters(source);
}

} // namespace

/// Combines the selected pages and validates optional destination consent before publication.
PageOps::Status PageOps::merge(const QList<MergeInput> &inputs, const QString &outPath,
                               QString *error, int *failedIndex,
                               const std::function<bool()> &mayCommit)
{
    if (failedIndex)
        *failedIndex = -1;
    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper odh(out);
        // Sources must outlive write() so their stream data can be copied. The
        // same path may appear as several inputs (the same file contributing two
        // different page ranges), so each distinct path is opened exactly once
        // and reused - both to save the parse and because two QPDFs over one file
        // would copy its shared objects into the output twice.
        std::vector<std::unique_ptr<QPDF>> sources;
        std::map<QString, QPDF *> opened;
        SourceForms sourceForms;
        FormCopy forms(out, sourceForms);
        MeasureDoc mergedMeasurements;
        int outputPage = 0;

        for (int i = 0; i < inputs.size(); ++i) {
            const MergeInput &in = inputs.at(i);
            auto it = opened.find(in.path);
            if (it == opened.end()) {
                auto src = std::make_unique<QPDF>();
                const Status st = open(*src, in.path, in.password, error);
                if (st != Status::Ok) {
                    if (failedIndex)
                        *failedIndex = i;
                    return st;
                }
                it = opened.emplace(in.path, src.get()).first;
                sources.push_back(std::move(src));
            }

            auto all = QPDFPageDocumentHelper(*it->second).getAllPages();
            const int n = static_cast<int>(all.size());
            QList<int> selected = in.pages;
            if (selected.isEmpty())
                for (int page = 0; page < n; ++page)
                    selected.append(page);
            for (int idx : selected) {
                if (idx < 0 || idx >= n) {
                    // The caller names the file (it has failedIndex), so this says
                    // only what the caller cannot know.
                    if (error) {
                        //: Merge or extract error, shown below a line that names the file.
                        //: %n is the file's page count, %1 the missing page number.
                        *error = tr("It has %n page(s); page %1 does not exist.", nullptr, n)
                                     .arg(idx + 1);
                    }
                    if (failedIndex)
                        *failedIndex = i;
                    return Status::Failed;
                }
                odh.addPage(all[static_cast<size_t>(idx)], false);
                forms.copied(*it->second, all[static_cast<size_t>(idx)]);
            }
            measurementPages::append(mergedMeasurements, measurementPages::read(*it->second),
                                     selected, outputPage);
            outputPage += selected.size();
        }
        forms.finish();
        measurementPages::write(out, mergedMeasurements);

        AtomicPdfWriter output(out, outPath);
        // Pages of one file keep its encryption. A merge of several files has none.
        if (sources.size() == 1)
            copyEncryption(output, *sources.front());
        output.write(mayCommit);
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

PageOps::Status PageOps::merge(const QStringList &inPaths, const QString &outPath, QString *error)
{
    QList<MergeInput> inputs;
    inputs.reserve(inPaths.size());
    for (const QString &p : inPaths)
        inputs.append(MergeInput{p, {}, QString()});
    return merge(inputs, outPath, error, nullptr);
}

PageOps::Status PageOps::split(const QString &inPath, const QString &outDir, const QString &baseName,
                               const QString &password, QStringList *outFiles, QString *error,
                               const std::function<bool(const QStringList &)> &mayWrite,
                               const std::function<bool(const QString &)> &mayCommit)
{
    QPDF q;
    const Status st = open(q, inPath, password, error);
    if (st != Status::Ok)
        return st;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        const QDir dir(outDir);
        QStringList paths;
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            paths << dir.filePath(QStringLiteral("%1-%2.pdf")
                                      .arg(baseName)
                                      .arg(i + 1, 3, 10, QLatin1Char('0')));
        if (mayWrite && !mayWrite(paths))
            return Status::Failed;
        const auto data = measurementPages::read(q);
        SourceForms sourceForms;
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            QPDF out;
            out.emptyPDF();
            FormCopy forms(out, sourceForms);
            QPDFPageDocumentHelper(out).addPage(all[static_cast<size_t>(i)], false);
            forms.copied(q, all[static_cast<size_t>(i)]);
            forms.finish();
            MeasureDoc selected;
            measurementPages::append(selected, data, {i}, 0);
            measurementPages::write(out, selected);
            const QString &path = paths.at(i);
            // A stopped job throws here, before it takes the next file.
            AtomicPdfWriter output(out, path);
            copyEncryption(output, q);
            output.write([&mayCommit, &path] { return !mayCommit || mayCommit(path); });
            if (outFiles)
                outFiles->append(path);
        }
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        return Status::Failed;
    }
}

} // namespace mervin
