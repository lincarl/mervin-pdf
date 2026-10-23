#include "security/PageOps.h"
#include "io/JobContext.h"
#include "recent/PathKey.h"
#include "security/AtomicPdfWriter.h"
#include "security/MeasurementPages.h"
#include "security/QpdfSource.h"
#include <algorithm>
#include <stdexcept>

#include <qpdf/QPDF.hh>
#include <qpdf/QPDFAcroFormDocumentHelper.hh>
#include <qpdf/QPDFFormFieldObjectHelper.hh>
#include <qpdf/QPDFMatrix.hh>
#include <qpdf/QPDFNameTreeObjectHelper.hh>
#include <qpdf/QPDFNumberTreeObjectHelper.hh>
#include <qpdf/QPDFOutlineDocumentHelper.hh>
#include <qpdf/QPDFPageDocumentHelper.hh>
#include <qpdf/QPDFPageObjectHelper.hh>

#include <QDir>

#include <atomic>
#include <exception>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mervin {

namespace {

using ObjectSet = std::set<QPDFObjGen>;

std::atomic<bool> failRetarget{false}; // see PageOps::testing::failNextRetarget
thread_local PageOps::testing::SplitProjection splitProjection;

// How deep the recursive walks below follow form fields, outline items, structure elements
// and action chains. qpdf's own form traversal stops at the same depth. A walk that removes
// or limits something cuts what lies deeper, and a chain that comes back to an object it
// has seen ends there, so nothing stays unchecked because a walk stopped. Walks that do not
// recurse, such as a climb through /Parent, reach each object once and need no limit.
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
/// goes with it. Below kMaxDepth levels everything goes but the widgets in `kept`.
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

    /// The fields that went, and what was below the depth limit but the widgets in `kept`.
    /// The descendants of a field that went that hold no widget of `dropped` are not listed,
    /// as nothing shows them.
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
        } else if (kids.isArray()) {
            // Deeper than the walk goes. What is below goes unread, and the field counts as
            // holding a dropped widget, so it stays only when it is a kept widget itself.
            dropBelow(kids);
            field.removeKey("/Kids");
            verdict.dropped = true;
        }
        verdict.stays = verdict.kept || (dropped_ && !verdict.dropped);
        if (verdict.stays)
            removed_.erase(id); // dropBelow() may have reached it below another field
        else
            removed_.insert(id);
        seen_[id] = verdict;
        return verdict;
    }

    // Adds the fields and widgets below `kids`, a /Kids array, to removed_, except the
    // widgets in `kept` and what visit() has reached. It does not recurse and reaches each
    // object once, so it needs no depth limit. A dictionary with a /Type other than /Annot
    // is not part of a field tree, and stays.
    void dropBelow(QPDFObjectHandle kids)
    {
        std::vector<QPDFObjectHandle> queue = kids.getArrayAsVector();
        while (!queue.empty()) {
            QPDFObjectHandle node = queue.back();
            queue.pop_back();
            if (!node.isIndirect() || !node.isDictionary() || seen_.count(node.getObjGen())
                || !below_.insert(node.getObjGen()).second)
                continue;
            QPDFObjectHandle type = node.getKey("/Type");
            if (!type.isNull() && !type.isNameAndEquals("/Annot"))
                continue;
            if (!kept_.count(node.getObjGen()))
                removed_.insert(node.getObjGen());
            QPDFObjectHandle more = node.getKey("/Kids");
            if (more.isArray())
                for (QPDFObjectHandle kid : more.aitems())
                    queue.push_back(kid);
        }
    }

    const ObjectSet &kept_;
    const ObjectSet *dropped_;
    std::map<QPDFObjGen, Verdict> seen_;
    ObjectSet below_; // what dropBelow() reached
    ObjectSet removed_;
};

/// The fields above `node` through /Parent, nearest first, up to the top of its tree, or up
/// to a field already listed, as in a loop. Fields have no /Type, which keeps the climb out
/// of anything else that a damaged /Parent might name.
std::vector<QPDFObjectHandle> fieldsAbove(QPDFObjectHandle node)
{
    std::vector<QPDFObjectHandle> above;
    ObjectSet seen;
    if (node.isIndirect())
        seen.insert(node.getObjGen());
    for (QPDFObjectHandle parent = node.getKey("/Parent");
         parent.isDictionary() && !parent.hasKey("/Type")
         && (!parent.isIndirect() || seen.insert(parent.getObjGen()).second);
         parent = parent.getKey("/Parent"))
        above.push_back(parent);
    return above;
}

/// Prunes the field tree of `widget` from its top, then each field above the widget on its
/// own, so a field whose parent's /Kids does not list it, as in a damaged file, is pruned as
/// well. A field that the walk from the top reached costs a lookup. Returns the fields above
/// the widget, nearest first.
std::vector<QPDFObjectHandle> pruneFieldsOf(QPDFObjectHandle widget, FieldPruner &pruner)
{
    const std::vector<QPDFObjectHandle> above = fieldsAbove(widget);
    if (above.empty())
        pruner.prune(widget);
    for (auto field = above.rbegin(); field != above.rend(); ++field)
        pruner.prune(*field);
    return above;
}

/// The objects of a Delete Pages source, collected before the pages go.
struct Removal
{
    ObjectSet pages;           // the deleted pages
    ObjectSet annotations;     // the indirect annotations that only deleted pages list
    ObjectSet keptAnnotations; // the indirect annotations that kept pages list
    std::vector<QPDFObjectHandle> keptWidgets; // the widgets that kept pages list
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

/// Sets /Count of `parent` from `items`, the items now below it. For the outline root at
/// `depth` 0 the count is every open item, and for an item it is its open descendants,
/// negative while the item is closed. An item without /Count stays without it.
void recountOutline(QPDFObjectHandle parent, const std::vector<QPDFObjectHandle> &items, int depth)
{
    long long visible = 0; // the items shown below `parent` while it is open
    for (QPDFObjectHandle item : items) {
        QPDFObjectHandle count = item.getKey("/Count");
        visible += 1 + (count.isInteger() ? std::max(0LL, count.getIntValue()) : 0);
    }
    QPDFObjectHandle count = parent.getKey("/Count");
    if (depth == 0 || (count.isInteger() && count.getIntValue() > 0))
        parent.replaceKey("/Count", QPDFObjectHandle::newInteger(visible));
    else if (count.isInteger() && count.getIntValue() < 0)
        parent.replaceKey("/Count", QPDFObjectHandle::newInteger(-visible));
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
    for (size_t i = 0; i < kept.size(); ++i) {
        if (i > 0)
            kept[i].replaceKey("/Prev", kept[i - 1]);
        else
            kept[i].removeKey("/Prev");
        if (i + 1 < kept.size())
            kept[i].replaceKey("/Next", kept[i + 1]);
        else
            kept[i].removeKey("/Next");
    }
    recountOutline(parent, kept, depth);
    return true;
}

/// Makes the direct items below `parent` indirect, as `parent` must be, so that the walks
/// above tell items apart by object number, and cuts what those walks would not check.
/// ISO 32000 wants indirect items, but viewers follow a direct /Next as well. A chain that
/// comes back to an item already seen, as in a loop, ends before it, and an item at the depth
/// limit loses the items below it. /Last, /Prev and /Parent that name something outside the
/// chain are set to the item the chain reached, so they cannot keep a bookmark of a deleted
/// page in the file. Recounts /Count where the items below changed, and returns whether
/// anything below `parent` changed.
bool makeOutlineIndirect(QPDF &q, QPDFObjectHandle parent, ObjectSet &seen, int depth)
{
    const auto same = [](QPDFObjectHandle a, QPDFObjectHandle b) {
        return a.isIndirect() && b.isIndirect() && a.getObjGen() == b.getObjGen();
    };
    QPDFObjectHandle holder = parent;
    const char *link = "/First";
    std::vector<QPDFObjectHandle> items;
    bool cut = false;
    for (QPDFObjectHandle item = parent.getKey("/First"); item.isDictionary();
         item = item.getKey("/Next")) {
        if (!item.isIndirect()) {
            item = q.makeIndirectObject(item);
            holder.replaceKey(link, item);
        }
        if (!seen.insert(item.getObjGen()).second) {
            holder.removeKey(link);
            cut = true;
            break;
        }
        if (item.hasKey("/Parent") && !same(item.getKey("/Parent"), parent))
            item.replaceKey("/Parent", parent);
        if (item.hasKey("/Prev") && (items.empty() || !same(item.getKey("/Prev"), items.back()))) {
            if (items.empty())
                item.removeKey("/Prev");
            else
                item.replaceKey("/Prev", items.back());
        }
        if (depth < kMaxDepth) {
            cut |= makeOutlineIndirect(q, item, seen, depth + 1);
        } else if (item.hasKey("/First") || item.hasKey("/Last")) {
            for (const char *key : {"/First", "/Last", "/Count"})
                item.removeKey(key);
            cut = true;
        }
        items.push_back(item);
        holder = item;
        link = "/Next";
    }
    if (items.empty()) {
        if (cut || parent.hasKey("/First") || parent.hasKey("/Last")) {
            for (const char *key : {"/First", "/Last", "/Count"})
                parent.removeKey(key);
            cut = true;
        }
    } else if (cut || !same(parent.getKey("/Last"), items.back())) {
        parent.replaceKey("/Last", items.back());
        recountOutline(parent, items, depth);
        cut = true;
    }
    return cut;
}

/// Removes the outline items that go only to a deleted page and have no items left below
/// them, as their titles can describe the page. makeOutlineIndirect cuts loops and items
/// deeper than the walks go, and repairs links that lead outside the outline, first. An
/// outline that qpdf cannot read stays as it was.
void pruneOutlines(QPDF &q, const ObjectSet &pages)
{
    QPDFObjectHandle outlines = q.getRoot().getKey("/Outlines");
    if (!outlines.isDictionary())
        return;
    if (!outlines.isIndirect()) { // as ISO 32000 wants, so its items' /Parent can name it
        outlines = q.makeIndirectObject(outlines);
        q.getRoot().replaceKey("/Outlines", outlines);
    }
    ObjectSet doomed;
    try {
        ObjectSet seen;
        makeOutlineIndirect(q, outlines, seen, 0);
        QPDFOutlineDocumentHelper names(q);
        seen.clear();
        findDeadOutlines(outlines, names, pages, seen, doomed, 0);
    } catch (const std::exception &) {
        return;
    }
    ObjectSet seen;
    if (!doomed.empty())
        unlinkOutlines(outlines, doomed, seen, 0);
}

/// Removes the named destinations that go to one of `pages`, from the catalog's /Dests and
/// from the /Names /Dests tree, as their names can describe the page. Call it after
/// pruneOutlines, which looks bookmarks up by these names. A damaged name tree stays as it
/// was.
void pruneDestinations(QPDF &q, const ObjectSet &pages)
{
    const auto deleted = [&pages](QPDFObjectHandle dest) {
        if (dest.isDictionary())
            dest = dest.getKey("/D");
        if (!dest.isArray() || dest.getArrayNItems() == 0)
            return false;
        QPDFObjectHandle page = dest.getArrayItem(0);
        return page.isIndirect() && pages.count(page.getObjGen()) > 0;
    };
    QPDFObjectHandle root = q.getRoot();
    QPDFObjectHandle dests = root.getKey("/Dests");
    if (dests.isDictionary())
        for (const std::string &name : dests.getKeys())
            if (deleted(dests.getKey(name)))
                dests.removeKey(name);
    QPDFObjectHandle names = root.getKey("/Names");
    QPDFObjectHandle tree = names.isDictionary() ? names.getKey("/Dests") : names;
    if (!tree.isDictionary())
        return;
    try {
        QPDFNameTreeObjectHelper helper(tree, q);
        std::vector<std::string> gone;
        for (const auto &[name, dest] : helper.getAsMap())
            if (deleted(dest))
                gone.push_back(name);
        for (const std::string &name : gone)
            helper.remove(name);
    } catch (const std::exception &) {
        // The names stay, with null in place of the page.
    }
}

/// Finds the structure elements that mark content only on deleted pages, and those below
/// kMaxDepth levels, which go unread. read() only looks, so a structure tree that qpdf
/// cannot read can be left as it was; apply() then removes those elements with their
/// /ParentTree and /IDTree entries, and takes the deleted pages' content items out of the
/// elements that stay.
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
        if (depth > kMaxDepth) {
            doomBelow(elem); // deeper than the walk goes, so it goes unread
            return Deleted;
        }
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
            else
                doomed_.erase(elem.getObjGen()); // doomBelow() may have reached it elsewhere
        }
        return place;
    }

    // Dooms `elem` and the elements below it that classify() has not placed. It does not
    // recurse and reaches each element once, so it needs no depth limit.
    void doomBelow(QPDFObjectHandle elem)
    {
        std::vector<QPDFObjectHandle> queue{elem};
        while (!queue.empty()) {
            QPDFObjectHandle node = queue.back();
            queue.pop_back();
            if (!isElement(node))
                continue;
            if (node.isIndirect()
                && (places_.count(node.getObjGen()) || !doomed_.insert(node.getObjGen()).second))
                continue; // placed, being placed, or doomed already
            forEachKid(node, QPDFObjectHandle::newNull(),
                       [&queue](QPDFObjectHandle kid, QPDFObjectHandle) { queue.push_back(kid); });
        }
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
                // read() went through the same elements at the same depth.
                if (kid.isIndirect() ? isDoomed(kid) : classify(kid, page, depth) == Deleted)
                    return true;
                if (depth <= kMaxDepth) // the kids beyond the limit are all Deleted
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
/// or /ActualText can describe the page, and those deeper than the walk goes. A structure
/// tree that qpdf cannot read stays as it was.
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
/// parents' /Kids, and returns the ones to replace with null. That includes the trees that
/// the widgets of kept pages reach through /Parent when /AcroForm does not list them, as in
/// files that Extract wrote before it copied forms. A field above such a widget is not
/// returned, even when the walk through /Kids did not reach the widget, as in a damaged
/// file, because the widget inherits its type, flags and name from it. Its other kids that
/// only deleted pages show go all the same. Removing a field also drops /XFA, whose data
/// holds every field's value and would no longer match the form.
ObjectSet pruneDeletedFields(QPDF &q, const Removal &removal)
{
    FieldPruner pruner(removal.keptAnnotations, &removal.annotations);
    QPDFObjectHandle form = q.getRoot().getKey("/AcroForm");
    if (form.isDictionary()) {
        pruner.pruneArray(form.getKey("/Fields"));
        pruner.pruneArray(form.getKey("/CO"));
    }
    ObjectSet used; // the fields above the widgets of kept pages
    // A tree that /Fields lists was pruned above, which makes this a lookup.
    for (QPDFObjectHandle widget : removal.keptWidgets)
        for (QPDFObjectHandle field : pruneFieldsOf(widget, pruner))
            if (field.isIndirect())
                used.insert(field.getObjGen());
    if (form.isDictionary() && !pruner.removed().empty())
        form.removeKey("/XFA");
    ObjectSet gone;
    for (const QPDFObjGen &id : pruner.removed())
        if (!used.count(id))
            gone.insert(id);
    return gone;
}

/// Whether pages of `q` can list images and fonts that other pages draw: they share a
/// resources dictionary or an /XObject dictionary, inherit resources from a /Pages node, or
/// list one indirect XObject or font in resources dictionaries of their own. Only then is it
/// worth reading every page's content to find what a page lists but does not use. The first
/// three cases are the test of qpdf's own page selection
/// (QPDFJob::shouldRemoveUnreferencedResources). The last one adds files whose pages each
/// carry a copy of one dictionary that lists every page's images, and files that share a
/// font or a logo, which costs a content read per page. The walk visits each page, form
/// XObject and resource entry once. Call it before addPage or removePage, which move
/// inherited resources onto the pages.
bool sharesResources(QPDF &q)
{
    try {
        ObjectSet nodes, resources, values;
        std::vector<QPDFObjectHandle> queue{q.getRoot().getKey("/Pages")};
        while (!queue.empty()) {
            QPDFObjectHandle node = queue.back();
            queue.pop_back();
            if (node.isIndirect() && !nodes.insert(node.getObjGen()).second)
                continue;
            QPDFObjectHandle dict = node.isStream() ? node.getDict() : node;
            if (!dict.isDictionary())
                continue;
            QPDFObjectHandle kids = dict.getKey("/Kids");
            if (kids.isArray()) {
                if (dict.hasKey("/Resources"))
                    return true;
                for (QPDFObjectHandle kid : kids.aitems())
                    queue.push_back(kid);
                continue;
            }
            // A page, or a form XObject that one draws.
            QPDFObjectHandle own = dict.getKey("/Resources");
            if (own.isIndirect() && !resources.insert(own.getObjGen()).second)
                return true;
            if (!own.isDictionary())
                continue;
            QPDFObjectHandle xobjects = own.getKey("/XObject");
            if (xobjects.isIndirect() && !resources.insert(xobjects.getObjGen()).second)
                return true;
            ObjectSet listed; // the indirect XObjects and fonts that this page or form lists
            for (const char *type : {"/XObject", "/Font"}) {
                QPDFObjectHandle dict = own.getKey(type);
                if (!dict.isDictionary())
                    continue;
                for (auto item : dict.ditems()) {
                    if (item.second.isIndirect())
                        listed.insert(item.second.getObjGen());
                    if (item.second.isFormXObject())
                        queue.push_back(item.second);
                }
            }
            for (const QPDFObjGen &id : listed)
                if (!values.insert(id).second)
                    return true;
        }
    } catch (const std::exception &) {
        // A page tree that qpdf cannot walk keeps its resources.
    }
    return false;
}

/// Removes from `page` the fonts and XObjects that its content does not use. qpdf gives
/// the page its own resources dictionary first, and leaves a page whose content it cannot
/// read as it was.
void trimResources(QPDFPageObjectHelper page)
{
    try {
        page.removeUnreferencedResources();
    } catch (const std::exception &) {
        // The page keeps resources it does not use.
    }
}

/// Trims the pages that merge() and split() copy when their source shares resources (see
/// sharesResources), as qpdf's page selection does. Otherwise a copied page brings along
/// the images and fonts of every page that shares its resources.
class ResourceTrim
{
public:
    /// Trims `page` of `source` before addPage copies it the first time.
    void before(QPDF &source, QPDFPageObjectHelper page)
    {
        auto it = shares_.find(&source);
        if (it == shares_.end())
            it = shares_.emplace(&source, sharesResources(source)).first;
        if (it->second && trimmed_.insert({&source, page.getObjectHandle().getObjGen()}).second)
            trimResources(page);
    }

private:
    std::map<QPDF *, bool> shares_;
    std::set<std::pair<QPDF *, QPDFObjGen>> trimmed_;
};

/// Leaves out of `q` what only the deleted pages of `pages` used, after
/// QPDFPageDocumentHelper::removePage took them out of the page tree, which is all it
/// does. The pages, the annotations that no kept page lists and the form fields that only
/// deleted pages show become null, so outlines, links, structure and other references
/// cannot keep their content. Outline items, named destinations and structure elements
/// that lead only to deleted pages go as well. With `shared` (see sharesResources, which
/// must look before removePage), the kept pages also lose the resources they do not use.
void removeDeletedContent(QPDF &q, const std::vector<QPDFPageObjectHelper> &pages,
                          const std::set<int> &drop, bool shared)
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
        if (!annots.isArray())
            continue;
        for (QPDFObjectHandle annot : annots.aitems()) {
            if (!annot.isDictionary())
                continue;
            if (annot.getKey("/StructParent").isInteger())
                keys.insert(annot.getKey("/StructParent").getIntValue());
            if (!deleted && annot.getKey("/Subtype").isNameAndEquals("/Widget"))
                removal.keptWidgets.push_back(annot);
        }
    }
    for (const QPDFObjGen &id : removal.keptAnnotations)
        removal.annotations.erase(id);
    for (long long key : keptKeys)
        removal.parentKeys.erase(key);
    pruneOutlines(q, removal.pages);
    pruneDestinations(q, removal.pages);
    pruneStructure(q, removal);
    const ObjectSet fields = pruneDeletedFields(q, removal);
    const ObjectSet *gone[] = {&removal.pages, &removal.annotations, &fields};
    for (const ObjectSet *objects : gone)
        for (const QPDFObjGen &id : *objects)
            q.replaceObject(id, QPDFObjectHandle::newNull());
    if (shared)
        for (QPDFPageObjectHelper page : QPDFPageDocumentHelper(q).getAllPages())
            trimResources(page);
}

// The fields of the trees of `fields`, with their fully qualified names. The walk reaches
// each field once, so it needs no depth limit.
std::vector<std::pair<QPDFObjectHandle, std::string>>
fieldNames(const std::vector<QPDFObjectHandle> &fields)
{
    std::vector<std::pair<QPDFObjectHandle, std::string>> names;
    ObjectSet seen;
    std::vector<QPDFObjectHandle> queue = fields;
    while (!queue.empty()) {
        QPDFObjectHandle node = queue.back();
        queue.pop_back();
        if (!node.isIndirect() || !node.isDictionary() || !seen.insert(node.getObjGen()).second)
            continue;
        if (node.hasKey("/T"))
            names.emplace_back(node, QPDFFormFieldObjectHelper(node).getFullyQualifiedName());
        QPDFObjectHandle kids = node.getKey("/Kids");
        if (kids.isArray())
            for (QPDFObjectHandle kid : kids.aitems())
                queue.push_back(kid);
    }
    return names;
}

/// Form helpers and action names for sources, each collected once.
class SourceForms
{
public:
    struct Names
    {
        std::set<std::string> fields, movies;
    };

    /// Retains full-source names even when a part copies only selected field branches.
    const Names &namesOf(QPDF &pdf)
    {
        auto [it, inserted] = names_.try_emplace(&pdf);
        if (inserted) {
            try {
                QPDFObjectHandle form = pdf.getRoot().getKey("/AcroForm");
                QPDFObjectHandle fields = form.isDictionary() ? form.getKey("/Fields") : form;
                if (fields.isArray())
                    for (const auto &named : fieldNames(fields.getArrayAsVector()))
                        it->second.fields.insert(named.second);
                for (QPDFObjectHandle page : pdf.getAllPages()) {
                    QPDFObjectHandle annots = page.getKey("/Annots");
                    if (annots.isArray())
                        for (QPDFObjectHandle annot : annots.aitems())
                            if (annot.isDictionary() && annot.getKey("/Subtype").isNameAndEquals("/Movie")
                                && annot.getKey("/T").isString())
                                it->second.movies.insert(annot.getKey("/T").getUTF8Value());
                }
            } catch (const std::exception &) {
                // As before, an unreadable name stays and finds nothing in the copy.
            }
        }
        return it->second;
    }

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
    std::map<QPDF *, Names> names_;
};

/// Builds independent, small form sources for Split without changing the original tree.
/// Cross-references outside /Parent, /Kids and annotation /P retain the existing copy path.
class SplitFormPages
{
public:
    SplitFormPages(QPDF &source, const std::vector<QPDFPageObjectHelper> &pages)
        : form_(source.getRoot().getKey("/AcroForm"))
    {
        try {
            QPDFObjectHandle fields = form_.isDictionary() ? form_.getKey("/Fields") : form_;
            if (!fields.isArray() || fields.getArrayNItems() == 0)
                return;
            for (QPDFObjectHandle field : fields.aitems())
                if (!index(field, QPDFObjectHandle::newNull(), 0))
                    return;
            for (auto page : pages) {
                QPDFObjectHandle object = page.getObjectHandle();
                pages_.insert(object.getObjGen());
                QPDFObjectHandle annots = object.getKey("/Annots");
                if (annots.isNull())
                    continue;
                if (!annots.isArray())
                    return;
                for (QPDFObjectHandle annot : annots.aitems()) {
                    if (!annot.isIndirect() || !annot.isDictionary())
                        return;
                    annotations_.insert(annot.getObjGen());
                    if (annot.getKey("/Subtype").isNameAndEquals("/Widget")
                        && !nodes_.count(annot.getObjGen()))
                        return;
                }
            }
            valid_ = true;
        } catch (const std::exception &) {
            // Malformed forms use the existing copy and pruning rules.
        }
    }

    /// Copies just this page's widgets and their ancestors, or declines the fast path.
    std::unique_ptr<QPDF> select(QPDFPageObjectHelper page)
    {
        if (!valid_)
            return {};
        QPDFObjectHandle original = page.getObjectHandle();
        QPDFObjectHandle annots = original.getKey("/Annots");
        if (!annots.isArray())
            return {};
        ObjectSet selected;
        for (QPDFObjectHandle annot : annots.aitems()) {
            if (!annot.getKey("/Subtype").isNameAndEquals("/Widget"))
                continue;
            auto id = annot.getObjGen();
            while (id.isIndirect() && selected.insert(id).second)
                id = nodes_.at(id).parent;
        }
        if (selected.empty())
            return {};
        // Validate dependencies before copying anything. Importing an action or an
        // appearance that refers back into the original form would copy that tree again
        // and give its references different identities from the selected branches.
        if (!safeProperties(form_, {"/Fields"})
            || !safeProperties(original, {"/Parent", "/Annots"}))
            return {};
        for (const char *key : {"/Resources", "/MediaBox", "/CropBox", "/Rotate"})
            if (!safe(page.getAttribute(key, false), 0))
                return {};
        for (const auto &id : selected)
            if (!safeProperties(nodes_.at(id).object, {"/Parent", "/Kids", "/P"}))
                return {};
        for (QPDFObjectHandle annot : annots.aitems())
            if (!nodes_.count(annot.getObjGen()) && !safeProperties(annot, {"/P"}))
                return {};

        auto part = std::make_unique<QPDF>();
        part->emptyPDF();
        QPDFObjectHandle copiedPage = part->makeIndirectObject(
            copyProperties(*part, original, {"/Parent", "/Annots"}));
        // addPage normally moves inherited attributes onto the page. This independent
        // page has no source /Parent, so retain those attributes explicitly.
        for (const char *key : {"/Resources", "/MediaBox", "/CropBox", "/Rotate"}) {
            QPDFObjectHandle value = page.getAttribute(key, false);
            if (!value.isNull())
                copiedPage.replaceKey(key, copyValue(*part, value));
        }
        QPDFPageDocumentHelper(*part).addPage(QPDFPageObjectHelper(copiedPage), false);
        std::map<QPDFObjGen, QPDFObjectHandle> copied;
        std::map<QPDFObjGen, std::vector<QPDFObjGen>> children;
        for (const auto &id : selected) {
            copied.emplace(id, part->makeIndirectObject(
                copyProperties(*part, nodes_.at(id).object, {"/Parent", "/Kids", "/P"})));
            children[nodes_.at(id).parent].push_back(id);
        }
        // Preserve source sibling order without scanning all of a parent's omitted kids.
        for (auto &[parent, kids] : children)
            std::sort(kids.begin(), kids.end(), [&](const auto &a, const auto &b) {
                return nodes_.at(a).order < nodes_.at(b).order;
            });
        const auto childArray = [&](const QPDFObjGen &parent) {
            auto result = QPDFObjectHandle::newArray();
            for (const auto &id : children[parent])
                result.appendItem(copied.at(id));
            return result;
        };
        for (auto &[id, object] : copied) {
            auto &node = nodes_.at(id);
            if (node.parent.isIndirect())
                object.replaceKey("/Parent", copied.at(node.parent));
            if (children.count(id))
                object.replaceKey("/Kids", childArray(id));
            if (node.object.hasKey("/P"))
                object.replaceKey("/P", copiedPage);
        }
        auto copiedAnnots = QPDFObjectHandle::newArray();
        for (QPDFObjectHandle annot : annots.aitems()) {
            const auto found = copied.find(annot.getObjGen());
            QPDFObjectHandle object = found != copied.end() ? found->second
                : part->makeIndirectObject(copyProperties(*part, annot, {"/P"}));
            if (annot.hasKey("/P"))
                object.replaceKey("/P", copiedPage);
            copiedAnnots.appendItem(object);
        }
        copiedPage.replaceKey("/Annots", copiedAnnots);
        auto form = copyProperties(*part, form_, {"/Fields"});
        form.replaceKey("/Fields", childArray(QPDFObjGen()));
        part->getRoot().replaceKey("/AcroForm", part->makeIndirectObject(form));
        ++splitProjection.pages;
        splitProjection.fields += selected.size();
        return part;
    }

private:
    struct Node
    {
        QPDFObjectHandle object;
        QPDFObjGen parent;
        size_t order;
    };

    /// Requires an indirect, acyclic tree with reciprocal parents and bounded depth.
    bool index(QPDFObjectHandle field, QPDFObjectHandle parent, int depth)
    {
        if (depth >= kMaxDepth || !field.isIndirect() || !field.isDictionary()
            || nodes_.count(field.getObjGen()))
            return false;
        QPDFObjectHandle declared = field.getKey("/Parent");
        if (parent.isNull() ? !declared.isNull() : !declared.isSameObjectAs(parent))
            return false;
        const bool widget = field.getKey("/Subtype").isNameAndEquals("/Widget");
        // Only annotations have a page link we can safely replace. Nonterminal widgets
        // and typed parents have ambiguous field inheritance, so keep the existing path.
        if ((!widget && (field.hasKey("/P") || field.hasKey("/Type")))
            || (widget && field.hasKey("/Kids")))
            return false;
        nodes_.emplace(field.getObjGen(), Node{field, parent.getObjGen(), nodes_.size()});
        QPDFObjectHandle kids = field.getKey("/Kids");
        if (kids.isNull())
            return true;
        if (!kids.isArray())
            return false;
        for (QPDFObjectHandle kid : kids.aitems())
            if (!index(kid, field, depth + 1))
                return false;
        return true;
    }

    /// Allows only dependencies that cannot pull pages, annotations or fields back in.
    bool safe(QPDFObjectHandle value, int depth)
    {
        if (depth >= kMaxDepth)
            return false;
        const bool indirect = value.isIndirect();
        const auto id = value.getObjGen();
        if (indirect) {
            if (nodes_.count(id) || annotations_.count(id) || pages_.count(id))
                return false;
            const auto [it, inserted] = safe_.emplace(id, false);
            if (!inserted)
                return it->second; // a cycle is conservatively ineligible
        }
        bool result = true;
        if (value.isStream())
            result = safe(value.getDict(), depth + 1);
        else if (value.isDictionary()) {
            if (value.getKey("/Type").isNameAndEquals("/Page")
                || value.getKey("/Type").isNameAndEquals("/Pages")
                || value.getKey("/Type").isNameAndEquals("/Catalog")
                || value.getKey("/Subtype").isNameAndEquals("/Widget")
                || (value.hasKey("/Subtype") && value.hasKey("/Rect")))
                result = false;
            else
                for (auto item : value.ditems())
                    if (!safe(item.second, depth + 1)) {
                        result = false;
                        break;
                    }
        } else if (value.isArray()) {
            for (QPDFObjectHandle item : value.aitems())
                if (!safe(item, depth + 1)) {
                    result = false;
                    break;
                }
        }
        if (indirect)
            safe_[id] = result;
        return result;
    }

    /// Checks nonstructural properties while leaving source dictionaries untouched.
    bool safeProperties(QPDFObjectHandle object, const std::set<std::string> &skip)
    {
        for (auto item : object.ditems())
            if (!skip.count(item.first) && !safe(item.second, 0))
                return false;
        return true;
    }

    /// Imports direct containers and uses qpdf's stream-preserving copier for references.
    static QPDFObjectHandle copyValue(QPDF &out, QPDFObjectHandle value)
    {
        if (value.isIndirect())
            return out.copyForeignObject(value);
        if (value.isDictionary())
            return copyProperties(out, value, {});
        if (value.isArray()) {
            auto result = QPDFObjectHandle::newArray();
            for (QPDFObjectHandle item : value.aitems())
                result.appendItem(copyValue(out, item));
            return result;
        }
        // Parsed scalar values can retain source ownership too, for example /Rotate.
        return value.shallowCopy();
    }

    /// Copies validated properties, excluding links supplied by the selected branch graph.
    static QPDFObjectHandle copyProperties(QPDF &out, QPDFObjectHandle object,
                                           const std::set<std::string> &skip)
    {
        auto result = QPDFObjectHandle::newDictionary();
        for (auto item : object.ditems())
            if (!skip.count(item.first))
                result.replaceKey(item.first, copyValue(out, item.second));
        return result;
    }

    QPDFObjectHandle form_;
    std::map<QPDFObjGen, Node> nodes_;
    ObjectSet annotations_, pages_;
    std::map<QPDFObjGen, bool> safe_;
    bool valid_ = false;
};

/// Copies form fields along with the pages that one output gets from QPDFPageDocumentHelper::
/// addPage, which copies only the page and leaves its fields out of the output's /AcroForm.
/// Call copied() after each addPage and finish() before writing.
///
/// Like fixCopiedAnnotations in qpdf's own page selection, finish() gives each copied page
/// its own annotations and fields, and renames a field whose name the output already has,
/// for example to "name+1". It calls transformAnnotations, the function behind
/// fixCopiedAnnotations, once for all pages of a source instead of once per page, because
/// each call copies the whole tree of a field that spans pages. Copied per page, a 200-page
/// form with one top-level field took minutes and gigabytes. A field shown on several
/// copied pages therefore stays one field. A page copied again goes in a later pass and
/// gets fields of its own, so the copies fill in separately.
///
/// transformAnnotations first copies each field tree whole with copyForeignObject, then
/// copies that copy field by field. Actions of the copied annotations, such as a reset
/// button's /Fields, still name the first copy, so copyBatch points them at the second, and
/// finish() empties what is left of the first copies. finish() also nulls the annotations of
/// left-out pages that a copied annotation names, such as the note that a reply answers.
class FormCopy
{
public:
    FormCopy(QPDF &out, SourceForms &sources, SourceForms *actionSources = nullptr)
        : out_(out), form_(out), sources_(sources),
          actionSources_(actionSources ? *actionSources : sources) {}

    /// Records that addPage just added `page` of `source` as the output's last page.
    void copied(QPDF &source, QPDFPageObjectHelper page, QPDF *originalSource = nullptr)
    {
        QPDFObjectHandle from = page.getObjectHandle();
        const int pass = passes_[{&source, from.getObjGen()}]++;
        copies_.push_back({&source, originalSource ? originalSource : &source, pass, from,
                           out_.getAllPages().back()});
    }

    /// Copies the forms of the recorded pages, then removes the copied fields that have no
    /// widget on an output page. A field tree that spans several pages comes along whole,
    /// and the output must not carry the values of pages it leaves out. A value on a parent
    /// that a copied widget shares stays.
    void finish()
    {
        copyForms();
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
        // Prunes every field tree that a widget in the output belongs to, whether a page
        // lists the widget or not. The widgets of a page whose form could not be copied reach
        // their field tree only through /Parent, and the first copies of field trees are
        // reachable only from actions that copyBatch could not point elsewhere. The fields
        // that go are emptied, except those above a widget that a page lists, which still
        // names them.
        ObjectSet used;
        for (QPDFObjectHandle page : out_.getAllPages()) {
            QPDFObjectHandle annots = page.getKey("/Annots");
            if (annots.isArray())
                for (QPDFObjectHandle annot : annots.aitems())
                    pruneTreeOf(annot, pruner, &used);
        }
        for (QPDFObjectHandle object : out_.getAllObjects())
            pruneTreeOf(object, pruner, nullptr);
        for (const QPDFObjGen &id : pruner.removed())
            if (!used.count(id))
                out_.replaceObject(id, QPDFObjectHandle::newNull());
        removeLeftOutAnnotations(widgets, used);
        scopeFormActions();
    }

private:
    /// Copies of field tree nodes and annotations, from a first copy to the copy that a page
    /// uses.
    using Copies = std::map<QPDFObjGen, QPDFObjectHandle>;

    /// What the references in the copied actions of one pass point at instead.
    struct Targets
    {
        Copies copies;
        std::map<std::string, std::string> names; // fully qualified names that a rename changed
    };

    // Prunes the field tree of `widget`, if it is a widget (see pruneFieldsOf), and adds the
    // fields above the widget to `used`.
    static void pruneTreeOf(QPDFObjectHandle widget, FieldPruner &pruner, ObjectSet *used)
    {
        if (!widget.isDictionary() || !widget.getKey("/Subtype").isNameAndEquals("/Widget"))
            return;
        const std::vector<QPDFObjectHandle> above = pruneFieldsOf(widget, pruner);
        if (used)
            for (QPDFObjectHandle field : above)
                if (field.isIndirect())
                    used->insert(field.getObjGen());
    }

    // addPage copies what a page refers to, so a reply's /IRT brings along the note it answers
    // from a page that the output leaves out, with the note's popup. Such annotations, which
    // no output page lists and whose /P is not an output page, become null, as Delete Pages
    // nulls the annotations of deleted pages. The reply then answers nothing. A popup goes
    // or stays with the annotation in its /Parent. `listed` holds the annotations that output
    // pages list, and `used` the fields that their widgets name.
    void removeLeftOutAnnotations(const ObjectSet &listed, const ObjectSet &used)
    {
        ObjectSet pages;
        for (QPDFObjectHandle page : out_.getAllPages())
            pages.insert(page.getObjGen());
        const auto keep = [&](QPDFObjectHandle annot) {
            if (annot.isIndirect() && (listed.count(annot.getObjGen()) || used.count(annot.getObjGen())))
                return true;
            QPDFObjectHandle page = annot.getKey("/P");
            return page.isIndirect() && pages.count(page.getObjGen()) > 0;
        };
        for (QPDFObjectHandle object : out_.getAllObjects()) {
            if (!object.isDictionary() || !object.getKey("/Subtype").isName()
                || !object.getKey("/Rect").isArray())
                continue; // not an annotation
            QPDFObjectHandle owner = object.getKey("/Subtype").isNameAndEquals("/Popup")
                ? object.getKey("/Parent")
                : object;
            if (!keep(object) && !(owner.isDictionary() && keep(owner)))
                out_.replaceObject(object.getObjGen(), QPDFObjectHandle::newNull());
        }
    }

    // The top of the field tree that `node` belongs to.
    static QPDFObjectHandle fieldTop(QPDFObjectHandle node)
    {
        const std::vector<QPDFObjectHandle> above = fieldsAbove(node);
        return above.empty() ? node : above.back();
    }

    // Adds to `copies` each node of the tree below `first` with the node at the same place
    // below `copy`, which transformAnnotations made from it.
    static void mapTree(QPDFObjectHandle first, QPDFObjectHandle copy, Copies &copies, int depth)
    {
        if (!first.isIndirect() || !copy.isIndirect() || first.getObjGen() == copy.getObjGen()
            || !copies.emplace(first.getObjGen(), copy).second || depth >= kMaxDepth)
            return;
        QPDFObjectHandle kids = first.getKey("/Kids");
        QPDFObjectHandle copiedKids = copy.getKey("/Kids");
        if (kids.isArray() && copiedKids.isArray())
            for (int i = 0; i < std::min(kids.getArrayNItems(), copiedKids.getArrayNItems()); ++i)
                mapTree(kids.getArrayItem(i), copiedKids.getArrayItem(i), copies, depth + 1);
    }

    // `fields`, a field reference, a fully qualified field name or an array of them, with
    // the references in `targets.copies` and the names in `targets.names` replaced. Returns
    // `fields` itself when nothing changes, and a new object otherwise.
    static QPDFObjectHandle mapFields(QPDFObjectHandle fields, const Targets &targets)
    {
        if (fields.isIndirect()) {
            const auto it = targets.copies.find(fields.getObjGen());
            return it == targets.copies.end() ? fields : it->second;
        }
        if (fields.isString()) {
            const auto it = targets.names.find(fields.getUTF8Value());
            return it == targets.names.end() ? fields
                                             : QPDFObjectHandle::newUnicodeString(it->second);
        }
        if (!fields.isArray())
            return fields;
        std::vector<QPDFObjectHandle> items = fields.getArrayAsVector();
        bool changed = false;
        for (QPDFObjectHandle &item : items) {
            QPDFObjectHandle mapped = mapFields(item, targets);
            if (!mapped.isSameObjectAs(item)) {
                item = mapped;
                changed = true;
            }
        }
        return changed ? QPDFObjectHandle::newArray(items) : fields;
    }

    // The entry of an action of type `type` that names an annotation by reference: /Annotation
    // of a Movie, /AN of a Rendition and /TA of a GoTo3DView or RichMediaExecute action, or
    // null for other types. A Movie can name its annotation by title in /T instead.
    static const char *annotationKey(QPDFObjectHandle type)
    {
        if (type.isNameAndEquals("/Movie"))
            return "/Annotation";
        if (type.isNameAndEquals("/Rendition"))
            return "/AN";
        if (type.isNameAndEquals("/GoTo3DView") || type.isNameAndEquals("/RichMediaExecute"))
            return "/TA";
        return nullptr;
    }

    // Whether a walk of an action chain ends at `action`, reached at `depth`: past the depth
    // limit, or back at an action in `seen`, as in a loop. Adds an indirect `action` that does
    // not end the walk to `seen`.
    static bool endsChain(QPDFObjectHandle action, ObjectSet &seen, int depth)
    {
        return depth >= kMaxDepth
            || (action.isIndirect() && !seen.insert(action.getObjGen()).second);
    }

    // `action` with its targets, and those of the actions in its /Next, mapped through
    // `targets`: the fields of a SubmitForm or ResetForm /Fields and of a Hide /T, and the
    // annotation that annotationKey() names. Returns `action` itself when nothing changes, and
    // a new object otherwise, so an action that several annotations share stays as it was. The
    // chain ends where endsChain() says, without the action there and what follows it, so the
    // result can be null.
    static QPDFObjectHandle mapAction(QPDFObjectHandle action, const Targets &targets,
                                      ObjectSet &seen, int depth)
    {
        if (!action.isDictionary() && !action.isArray())
            return action;
        if (endsChain(action, seen, depth))
            return QPDFObjectHandle::newNull();
        if (action.isArray()) { // a /Next that lists several actions
            std::vector<QPDFObjectHandle> items;
            bool changed = false;
            for (QPDFObjectHandle item : action.aitems()) {
                QPDFObjectHandle mapped = mapAction(item, targets, seen, depth + 1);
                changed |= !mapped.isSameObjectAs(item);
                if (!mapped.isNull())
                    items.push_back(mapped);
            }
            if (!changed)
                return action;
            return items.empty() ? QPDFObjectHandle::newNull() : QPDFObjectHandle::newArray(items);
        }
        QPDFObjectHandle result;
        const auto change = [&](const char *key, QPDFObjectHandle old, QPDFObjectHandle mapped) {
            if (mapped.isSameObjectAs(old))
                return;
            if (!result.isInitialized())
                result = action.shallowCopy();
            if (mapped.isNull())
                result.removeKey(key);
            else
                result.replaceKey(key, mapped);
        };
        QPDFObjectHandle type = action.getKey("/S");
        const char *key = type.isNameAndEquals("/Hide") ? "/T"
            : type.isNameAndEquals("/SubmitForm") || type.isNameAndEquals("/ResetForm")
            ? "/Fields"
            : annotationKey(type); // a Movie's /T is a title, not a field name
        if (key) {
            QPDFObjectHandle value = action.getKey(key);
            change(key, value, mapFields(value, targets));
        }
        QPDFObjectHandle next = action.getKey("/Next");
        change("/Next", next, mapAction(next, targets, seen, depth + 1));
        return result.isInitialized() ? result : action;
    }

    // Replaces the /A action and each /AA action of `holder` with what `rewrite` makes of
    // it, when that is a new object. `rewrite` can also return null, which removes the key,
    // or an array of actions, which then run one after the other (see asOneAction).
    template <typename Rewrite>
    static void rewriteActions(QPDFObjectHandle holder, Rewrite rewrite)
    {
        QPDFObjectHandle action = holder.getKey("/A");
        QPDFObjectHandle mapped = rewrite(action);
        if (!mapped.isSameObjectAs(action)) {
            mapped = asOneAction(mapped);
            if (mapped.isNull())
                holder.removeKey("/A");
            else
                holder.replaceKey("/A", mapped);
        }
        QPDFObjectHandle triggers = holder.getKey("/AA");
        if (!triggers.isDictionary())
            return;
        QPDFObjectHandle result;
        for (const auto &[key, value] : triggers.ditems()) {
            QPDFObjectHandle mappedValue = rewrite(value);
            if (mappedValue.isSameObjectAs(value))
                continue;
            if (!result.isInitialized())
                result = triggers.shallowCopy();
            mappedValue = asOneAction(mappedValue);
            if (mappedValue.isNull())
                result.removeKey(key);
            else
                result.replaceKey(key, mappedValue);
        }
        if (!result.isInitialized())
            return;
        if (result.getKeys().empty())
            holder.removeKey("/AA");
        else
            holder.replaceKey("/AA", result);
    }

    // One action for a place that takes one, such as /A, that runs `actions` when it is an
    // array of them. It is the first action, with the others after its own /Next, which
    // keeps the order in which a viewer runs them. Returns null for an empty array.
    static QPDFObjectHandle asOneAction(QPDFObjectHandle actions)
    {
        if (!actions.isArray())
            return actions;
        std::vector<QPDFObjectHandle> items = actions.getArrayAsVector();
        if (items.empty())
            return QPDFObjectHandle::newNull();
        QPDFObjectHandle head = items.front();
        if (items.size() == 1 || !head.isDictionary())
            return head;
        head = head.shallowCopy();
        std::vector<QPDFObjectHandle> next;
        QPDFObjectHandle own = head.getKey("/Next");
        if (own.isArray())
            next = own.getArrayAsVector();
        else if (own.isDictionary())
            next.push_back(own);
        next.insert(next.end(), items.begin() + 1, items.end());
        head.replaceKey("/Next", QPDFObjectHandle::newArray(next));
        return head;
    }

    // Maps the fields in the /A and /AA actions of `holder` through `targets`.
    static void mapActions(QPDFObjectHandle holder, const Targets &targets)
    {
        rewriteActions(holder, [&targets](QPDFObjectHandle action) {
            ObjectSet seen;
            return mapAction(action, targets, seen, 0);
        });
    }

    // A SubmitForm or ResetForm action without /Fields, with an empty /Fields, or with the
    // Exclude flag (bit 1 of /Flags) acts on every field of the document, or every field but
    // the listed ones, as viewers such as PDF.js read an empty list as every field. In an
    // output with pages from several files, which holds the fields of all of them, such an
    // action copied from one file would send or clear the fields of the others. So each one
    // becomes an explicit list of the fields copied from its own file, without the excluded
    // ones and without the flag. One that would list no field goes, and the actions after it
    // in its /Next take its place. In every output, entries that name a field the copy left
    // out are dropped, so an action whose listed fields are all left out goes too, and an
    // Exclude list that names none of them then excludes nothing. No copied action ends
    // with an empty /Fields unless its source had one and the output has one file.
    //
    // A Hide action names fields by fully qualified name or reference, and annotations by
    // reference, in /T. A Movie, Rendition, GoTo3DView or RichMediaExecute action names an
    // annotation by reference, and a Movie can name one by title instead. In an output with
    // several files, a name could find a field or annotation of another file, so these keep
    // only the targets of their own file, and go when none remains, with their /Next taking
    // their place. A Movie title then becomes a reference to its own movie annotation, as
    // nothing renames those and another file can have the same title. An output with one
    // file drops only the targets that it left out, and keeps a name that named nothing in
    // the source, as it acts on nothing either way.
    //
    // The actions of the pages, their annotations and the fields above their widgets are
    // covered, with their /AA and /Next. JavaScript and ImportData actions cannot be limited
    // this way. A /Next chain ends after kMaxDepth actions, or where it comes back to an
    // action.
    void scopeFormActions()
    {
        std::map<QPDF *, std::vector<QPDFObjectHandle>> pagesOf;
        std::map<QPDF *, QPDF *> originals;
        ObjectSet seenPages;
        for (const Copy &copy : copies_) {
            QPDFObjectHandle page = copy.to;
            originals[copy.source] = copy.originalSource;
            if (seenPages.insert(page.getObjGen()).second)
                pagesOf[copy.source].push_back(page);
        }
        const bool several = pagesOf.size() > 1;
        for (auto &[source, pages] : pagesOf) {
            std::vector<QPDFObjectHandle> tops, holders; // its top-level fields, what has actions
            ObjectSet seen, seenTops, annotations;
            std::map<std::string, QPDFObjectHandle> movies; // its movie annotations by title
            const auto hold = [&holders, &seen](QPDFObjectHandle object) {
                if (object.isIndirect() && seen.insert(object.getObjGen()).second)
                    holders.push_back(object);
            };
            for (QPDFObjectHandle page : pages) {
                hold(page);
                QPDFObjectHandle annots = page.getKey("/Annots");
                if (!annots.isArray())
                    continue;
                for (QPDFObjectHandle annot : annots.aitems()) {
                    if (!annot.isDictionary())
                        continue;
                    hold(annot);
                    if (annot.isIndirect())
                        annotations.insert(annot.getObjGen());
                    if (annot.isIndirect() && annot.getKey("/Subtype").isNameAndEquals("/Movie")
                        && annot.getKey("/T").isString())
                        movies.emplace(annot.getKey("/T").getUTF8Value(), annot);
                    if (!annot.getKey("/Subtype").isNameAndEquals("/Widget"))
                        continue;
                    QPDFObjectHandle node = annot;
                    for (QPDFObjectHandle field : fieldsAbove(annot)) {
                        node = field;
                        hold(node);
                    }
                    if (node.isIndirect() && seenTops.insert(node.getObjGen()).second)
                        tops.push_back(node);
                }
            }
            OwnTargets own(tops);
            own.annotations = std::move(annotations);
            own.movies = std::move(movies);
            own.single = !several;
            if (own.single)
                own.sourceNames = &actionSources_.namesOf(*originals.at(source));
            for (QPDFObjectHandle holder : holders)
                rewriteActions(holder, [&](QPDFObjectHandle action) {
                    ObjectSet seenActions;
                    return scopeAction(action, tops, own, several, seenActions, 0);
                });
        }
    }

    /// What the actions of one file in an output may name: its fields, by object and by fully
    /// qualified name, and the annotations of its pages, with its movie annotations by title.
    struct OwnTargets
    {
        // Reaches each field once, so it needs no depth limit.
        explicit OwnTargets(const std::vector<QPDFObjectHandle> &tops)
        {
            std::vector<QPDFObjectHandle> queue = tops;
            while (!queue.empty()) {
                QPDFObjectHandle node = queue.back();
                queue.pop_back();
                if (!node.isIndirect() || !node.isDictionary() || !ids.insert(node.getObjGen()).second)
                    continue;
                if (node.hasKey("/T"))
                    names.insert(QPDFFormFieldObjectHelper(node).getFullyQualifiedName());
                QPDFObjectHandle kids = node.getKey("/Kids");
                if (kids.isArray())
                    for (QPDFObjectHandle kid : kids.aitems())
                        queue.push_back(kid);
            }
        }

        /// Whether `entry` of a /Fields array names one of the fields.
        bool has(QPDFObjectHandle entry) const
        {
            if (entry.isIndirect())
                return ids.count(entry.getObjGen()) > 0;
            return entry.isString() && names.count(entry.getUTF8Value()) > 0;
        }

        /// Whether a Hide action keeps `entry` of its /T: one of the fields or annotations, or
        /// in an output with one file, a name that named no field of the source.
        bool hides(QPDFObjectHandle entry) const
        {
            if (entry.isIndirect())
                return ids.count(entry.getObjGen()) > 0 || annotations.count(entry.getObjGen()) > 0;
            if (!entry.isString())
                return false;
            const std::string name = entry.getUTF8Value();
            return names.count(name) > 0 || (single && sourceNames && !sourceNames->fields.count(name));
        }

        /// Whether an action keeps `annotation`, the annotation that it names by reference.
        bool shows(QPDFObjectHandle annotation) const
        {
            return annotation.isIndirect() && annotations.count(annotation.getObjGen()) > 0;
        }

        ObjectSet ids;
        std::set<std::string> names;
        ObjectSet annotations;
        std::map<std::string, QPDFObjectHandle> movies; // the first of each title
        bool single = false; // the output has pages of this file only
        const SourceForms::Names *sourceNames = nullptr; // full source, only for a single file
    };

    // `action`, and the actions in its /Next, with each SubmitForm, ResetForm, Hide and
    // annotation action (see annotationKey) limited as scopeFormActions says, to the trees
    // `tops` of one file whose targets are `own`, and `several` when the output has more than
    // one file. Returns `action` itself when nothing changes, and a new object otherwise. An
    // action that is left out gives way to its /Next, so the result can be null when nothing
    // follows, or an array of actions. The chain ends where endsChain() says, without the
    // action there and what follows it, in every output.
    static QPDFObjectHandle scopeAction(QPDFObjectHandle action,
                                        const std::vector<QPDFObjectHandle> &tops,
                                        const OwnTargets &own, bool several, ObjectSet &seen,
                                        int depth)
    {
        if (!action.isDictionary() && !action.isArray())
            return action;
        if (endsChain(action, seen, depth))
            return QPDFObjectHandle::newNull();
        if (action.isArray()) { // a /Next that lists several actions
            std::vector<QPDFObjectHandle> items;
            bool changed = false;
            for (QPDFObjectHandle item : action.aitems()) {
                QPDFObjectHandle scoped = scopeAction(item, tops, own, several, seen, depth + 1);
                if (scoped.isSameObjectAs(item)) {
                    items.push_back(item);
                    continue;
                }
                changed = true;
                if (scoped.isArray())
                    for (QPDFObjectHandle each : scoped.aitems())
                        items.push_back(each);
                else if (!scoped.isNull())
                    items.push_back(scoped);
            }
            if (!changed)
                return action;
            return items.empty() ? QPDFObjectHandle::newNull() : QPDFObjectHandle::newArray(items);
        }
        QPDFObjectHandle next = action.getKey("/Next");
        QPDFObjectHandle scopedNext = scopeAction(next, tops, own, several, seen, depth + 1);
        QPDFObjectHandle result;
        const auto change = [&](const char *key, QPDFObjectHandle value) {
            if (!result.isInitialized())
                result = action.shallowCopy();
            if (value.isNull())
                result.removeKey(key);
            else
                result.replaceKey(key, value);
        };
        QPDFObjectHandle type = action.getKey("/S");
        // A missing entry is a direct null. A reference to an object that finish() nulled
        // is not, and names a target that the copy left out.
        const auto missing = [](QPDFObjectHandle value) { return !value.isIndirect() && value.isNull(); };
        if (type.isNameAndEquals("/Hide")) {
            QPDFObjectHandle targets = action.getKey("/T");
            if (!missing(targets)) {
                const std::vector<QPDFObjectHandle> listed = targets.isArray()
                    ? targets.getArrayAsVector()
                    : std::vector<QPDFObjectHandle>{targets};
                std::vector<QPDFObjectHandle> kept;
                for (QPDFObjectHandle entry : listed)
                    if (own.hides(entry))
                        kept.push_back(entry);
                if (kept.empty())
                    return scopedNext; // it would hide nothing, so what follows takes its place
                if (kept.size() < listed.size())
                    change("/T", QPDFObjectHandle::newArray(kept));
            }
        } else if (const char *key = annotationKey(type)) {
            QPDFObjectHandle annotation = action.getKey(key);
            QPDFObjectHandle title = action.getKey("/T");
            if (!missing(annotation)) {
                if (!own.shows(annotation))
                    return scopedNext; // its annotation is not one of its own file's
            } else if (type.isNameAndEquals("/Movie") && title.isString()) {
                const auto found = own.movies.find(title.getUTF8Value());
                if (several) {
                    // Nothing renames movie annotations, so another file can have one of the
                    // same title. The action names its own one by reference instead.
                    if (found == own.movies.end())
                        return scopedNext;
                    change("/T", QPDFObjectHandle::newNull());
                    change("/Annotation", found->second);
                } else if (found == own.movies.end() && own.sourceNames
                           && own.sourceNames->movies.count(title.getUTF8Value())) {
                    return scopedNext; // its movie annotation was left out
                }
            }
        }
        if (type.isNameAndEquals("/SubmitForm") || type.isNameAndEquals("/ResetForm")) {
            QPDFObjectHandle flags = action.getKey("/Flags");
            const long long bits = flags.isInteger() ? flags.getIntValue() : 0;
            const bool exclude = bits & 1;
            QPDFObjectHandle fields = action.getKey("/Fields");
            const int listed = fields.isArray() ? fields.getArrayNItems() : 0;
            std::vector<QPDFObjectHandle> kept; // the listed entries that name a field of `own`
            for (int i = 0; i < listed; ++i)
                if (own.has(fields.getArrayItem(i)))
                    kept.push_back(fields.getArrayItem(i));
            // The action covers every field of the document when it has no list, an empty
            // one, or an Exclude list whose fields the copy all left out.
            const bool every = listed == 0 || (exclude && kept.empty());
            if (listed > 0 && kept.empty() && !exclude)
                return scopedNext; // its fields are all left out, so it acts on none
            if (several && (every || exclude)) {
                const std::vector<QPDFObjectHandle> list =
                    every ? tops : withoutExcluded(tops, kept);
                if (list.empty())
                    return scopedNext; // what follows takes its place
                change("/Fields", QPDFObjectHandle::newArray(list));
                if (exclude)
                    change("/Flags", QPDFObjectHandle::newInteger(bits & ~1LL));
            } else if (listed > 0 && every) {
                // One file whose excluded fields are all left out. Without a list the copy
                // acts on every field it has, which are the ones the source acted on.
                change("/Fields", QPDFObjectHandle::newNull());
                change("/Flags", QPDFObjectHandle::newInteger(bits & ~1LL));
            } else if (static_cast<int>(kept.size()) < listed) {
                change("/Fields", QPDFObjectHandle::newArray(kept)); // without the fields left out
            }
        }
        if (!scopedNext.isSameObjectAs(next))
            change("/Next", scopedNext);
        return result.isInitialized() ? result : action;
    }

    /// Fields that an Exclude list names, by reference and by fully qualified name.
    struct Excluded
    {
        ObjectSet ids;
        std::set<std::string> names;
    };

    // The fields of the trees `tops` without the ones that `excluded` lists, by reference or
    // fully qualified name, and their descendants. A field with nothing excluded below it
    // stands for its whole tree.
    static std::vector<QPDFObjectHandle>
    withoutExcluded(const std::vector<QPDFObjectHandle> &tops,
                    const std::vector<QPDFObjectHandle> &excluded)
    {
        Excluded gone;
        for (QPDFObjectHandle item : excluded) {
            if (item.isIndirect())
                gone.ids.insert(item.getObjGen());
            else if (item.isString())
                gone.names.insert(item.getUTF8Value());
        }
        std::vector<QPDFObjectHandle> result;
        ObjectSet seen;
        for (QPDFObjectHandle top : tops)
            if (addUnexcluded(top, gone, seen, result, 0))
                result.push_back(top);
        return result;
    }

    // Whether nothing in the tree of `node` is excluded, so that `node` can stand for it.
    // Otherwise adds the fields below it that can stand for their own trees to `result`. A
    // field reached twice, as in a loop, counts as excluded, and so does one at the depth
    // limit with fields below it, so the list leaves out what the walk did not check.
    static bool addUnexcluded(QPDFObjectHandle node, const Excluded &gone, ObjectSet &seen,
                              std::vector<QPDFObjectHandle> &result, int depth)
    {
        if (!node.isIndirect() || !node.isDictionary())
            return true; // qpdf skips it in a field tree too
        if (!seen.insert(node.getObjGen()).second || gone.ids.count(node.getObjGen())
            || (node.hasKey("/T")
                && gone.names.count(QPDFFormFieldObjectHelper(node).getFullyQualifiedName())))
            return false;
        QPDFObjectHandle kids = node.getKey("/Kids");
        if (!kids.isArray() || kids.getArrayNItems() == 0)
            return true;
        if (depth >= kMaxDepth)
            return false;
        std::vector<QPDFObjectHandle> parts; // what stands for the kids' trees
        bool whole = true;
        for (QPDFObjectHandle kid : kids.aitems()) {
            if (!addUnexcluded(kid, gone, seen, parts, depth + 1))
                whole = false;
            else if (kid.isIndirect() && (kid.hasKey("/T") || kid.hasKey("/Kids")))
                parts.push_back(kid); // a field, not a bare widget
        }
        if (!whole)
            result.insert(result.end(), parts.begin(), parts.end());
        return whole;
    }

    // The fields of `before` that addAndRenameFormFields renamed, from the old fully
    // qualified name to the new one.
    static std::map<std::string, std::string>
    renamed(const std::vector<std::pair<QPDFObjectHandle, std::string>> &before)
    {
        std::map<std::string, std::string> names;
        for (const auto &[field, name] : before) {
            QPDFObjectHandle node = field; // qpdf 11 reads through non-const handles
            const std::string now = QPDFFormFieldObjectHelper(node).getFullyQualifiedName();
            if (now != name)
                names.emplace(name, now);
        }
        return names;
    }

    // Points the references of `made`, the copies of the source annotations in `annots`, at
    // the copies instead of the first copies: the fields in the actions of the annotations and
    // their fields, by reference or by a name in `names`, and the annotations in /IRT, /Popup
    // and a popup's /Parent.
    void retarget(QPDFObjectHandle annots, const std::vector<QPDFObjectHandle> &made,
                  std::map<std::string, std::string> names)
    {
        if (failRetarget.exchange(false))
            throw std::runtime_error("retarget failed for a test");
        Targets targets;
        targets.names = std::move(names);
        Copies &copies = targets.copies;
        for (int i = 0; i < annots.getArrayNItems(); ++i) {
            QPDFObjectHandle annot = annots.getArrayItem(i);
            if (!annot.isIndirect())
                continue;
            // transformAnnotations copied each indirect annotation with copyForeignObject,
            // which returns that copy again.
            QPDFObjectHandle first = out_.copyForeignObject(annot);
            QPDFObjectHandle copy = made[static_cast<size_t>(i)];
            if (copy.getKey("/Subtype").isNameAndEquals("/Widget"))
                mapTree(fieldTop(first), fieldTop(copy), copies, 0);
            else
                mapTree(first, copy, copies, 0);
        }
        if (copies.empty() && targets.names.empty())
            return;
        // Each annotation and field once, so a name is not mapped twice.
        std::map<QPDFObjGen, QPDFObjectHandle> holders;
        for (QPDFObjectHandle annot : made) {
            holders.emplace(annot.getObjGen(), annot);
            const bool popup = annot.getKey("/Subtype").isNameAndEquals("/Popup");
            for (const char *key : {"/IRT", "/Popup", "/Parent"}) {
                QPDFObjectHandle target = annot.getKey(key);
                if ((popup || std::string(key) != "/Parent") && target.isIndirect())
                    if (const auto it = copies.find(target.getObjGen()); it != copies.end())
                        annot.replaceKey(key, it->second);
            }
        }
        for (auto &[first, copy] : copies)
            holders.emplace(copy.getObjGen(), copy);
        for (auto &[id, holder] : holders)
            mapActions(holder, targets);
    }

    // Removes /IRT, /Popup and a popup's /Parent from the annotations of `made` where they
    // still name the first copy of an annotation in `annots`, after retarget failed. Such a
    // reply then answers nothing, like one whose note is left out.
    void dropFirstCopyLinks(QPDFObjectHandle annots, const std::vector<QPDFObjectHandle> &made)
    {
        ObjectSet firsts;
        for (QPDFObjectHandle annot : annots.aitems())
            if (annot.isIndirect())
                firsts.insert(out_.copyForeignObject(annot).getObjGen());
        for (QPDFObjectHandle annot : made) {
            const bool popup = annot.getKey("/Subtype").isNameAndEquals("/Popup");
            for (const char *key : {"/IRT", "/Popup", "/Parent"}) {
                QPDFObjectHandle target = annot.getKey(key);
                if ((popup || std::string(key) != "/Parent") && target.isIndirect()
                    && firsts.count(target.getObjGen()))
                    annot.removeKey(key);
            }
        }
    }

    /// A page that addPage copied into the output.
    struct Copy
    {
        QPDF *source;
        QPDF *originalSource; // unabridged source for field and annotation action names
        int pass; // 0 for the first copy of a source page, 1 for the second, and so on
        QPDFObjectHandle from;
        QPDFObjectHandle to;
    };

    // Copies the forms of each source's pages one pass at a time, in the order they came.
    void copyForms()
    {
        std::vector<std::pair<QPDF *, int>> order;
        std::map<std::pair<QPDF *, int>, std::vector<Copy>> batches;
        for (const Copy &copy : copies_) {
            std::vector<Copy> &batch = batches[{copy.source, copy.pass}];
            if (batch.empty())
                order.emplace_back(copy.source, copy.pass);
            batch.push_back(copy);
        }
        for (const auto &key : order) {
            QPDFAcroFormDocumentHelper *from = sources_.of(*key.first);
            if (!from)
                continue;
            std::vector<Copy> &batch = batches[key];
            if (!copyBatch(batch, *from))
                for (Copy &copy : batch)
                    copyPage(copy, *from);
        }
    }

    // Gives the pages of `batch`, distinct pages of one source, their own annotations and
    // fields in one transformAnnotations call. When qpdf throws, the pages stay as addPage
    // copied them and this returns false.
    // Handles are not const, as qpdf 11 reads objects through non-const methods.
    bool copyBatch(std::vector<Copy> &batch, QPDFAcroFormDocumentHelper &from)
    {
        QPDFObjectHandle annots = QPDFObjectHandle::newArray();
        std::vector<int> counts; // how many of `annots` each page has
        for (Copy &copy : batch) {
            int count = 0;
            QPDFObjectHandle list = copy.from.getKey("/Annots");
            if (list.isArray())
                // qpdf skips anything else, which would break the counts.
                for (QPDFObjectHandle annot : list.aitems())
                    if (annot.isDictionary()) {
                        annots.appendItem(annot);
                        ++count;
                    }
            counts.push_back(count);
        }
        if (annots.getArrayNItems() == 0)
            return true;
        std::vector<QPDFObjectHandle> plain;
        for (Copy &copy : batch)
            plain.push_back(copy.to.getKey("/Annots"));
        try {
            std::vector<QPDFObjectHandle> made, fields;
            std::set<QPDFObjGen> sourceFields;
            form_.transformAnnotations(annots, made, fields, sourceFields, QPDFMatrix(),
                                       &from.getQPDF(), &from);
            if (made.size() != static_cast<size_t>(annots.getArrayNItems()))
                throw std::runtime_error("qpdf left out annotations");
            auto next = made.cbegin();
            for (size_t i = 0; i < batch.size(); ++i) {
                if (counts[i] == 0)
                    continue; // as fixCopiedAnnotations leaves a page without annotations
                batch[i].to.replaceKey("/Annots", QPDFObjectHandle::newArray(
                                                      std::vector(next, next + counts[i])));
                next += counts[i];
                pointAt(batch[i].to);
            }
            std::vector<std::pair<QPDFObjectHandle, std::string>> names;
            try {
                names = fieldNames(fields);
            } catch (const std::exception &) {
                // Names in the actions keep naming the fields as before.
            }
            form_.addAndRenameFormFields(fields);
            try {
                retarget(annots, made, renamed(names));
            } catch (const std::exception &) {
                // An action that still names a first copy's field acts on nothing once
                // finish() empties it. The first copies of annotations stay, as their /P is
                // an output page, so the links to them go.
                dropFirstCopyLinks(annots, made);
            }
            return true;
        } catch (const std::exception &) {
            for (size_t i = 0; i < batch.size(); ++i)
                restore(batch[i].to, plain[i]);
            return false;
        }
    }

    // Copies one page's form with fixCopiedAnnotations, after copyBatch failed. qpdf before
    // 12.4 can throw on damaged annotations; like qpdf, keep such a page as addPage copied it.
    void copyPage(Copy &copy, QPDFAcroFormDocumentHelper &from)
    {
        QPDFObjectHandle plain = copy.to.getKey("/Annots");
        try {
            form_.fixCopiedAnnotations(copy.to, copy.from, from);
        } catch (const std::exception &) {
            restore(copy.to, plain);
            return;
        }
        pointAt(copy.to);
    }

    // Points the annotations of `page` at it. transformAnnotations leaves /P on the page
    // that addPage copied first, which is another page when a page is copied again, and so
    // does fixCopiedAnnotations before qpdf 12.3.
    static void pointAt(QPDFObjectHandle page)
    {
        QPDFObjectHandle annots = page.getKey("/Annots");
        if (annots.isArray())
            for (QPDFObjectHandle annot : annots.aitems())
                if (annot.isDictionary() && annot.hasKey("/P"))
                    annot.replaceKey("/P", page);
    }

    // Puts back the /Annots that addPage gave `page`.
    static void restore(QPDFObjectHandle page, QPDFObjectHandle plain)
    {
        if (plain.isNull())
            page.removeKey("/Annots");
        else
            page.replaceKey("/Annots", plain);
    }

    QPDF &out_;
    QPDFAcroFormDocumentHelper form_;
    SourceForms &sources_;
    SourceForms &actionSources_;
    std::map<std::pair<QPDF *, QPDFObjGen>, int> passes_; // copies so far of each source page
    std::vector<Copy> copies_;
};

} // namespace

int PageOps::pageCount(const QString &path, const QString &password)
{
    int count = -1;
    return probe(path, &count, password) == Status::Ok ? count : -1;
}

QString PageOps::measurementsTooLargeError()
{
    return measurementPages::tooLargeError();
}

void PageOps::testing::failNextRetarget()
{
    failRetarget = true;
}

PageOps::testing::SplitProjection PageOps::testing::lastSplitProjection()
{
    return splitProjection;
}

PageOps::Status PageOps::probe(const QString &path, int *count, const QString &password,
                               QString *error, bool *encrypted, QString *sourceKey)
{
    std::unique_ptr<QPDF> source;
    std::shared_ptr<QpdfReadState> reads;
    const Status st = openQpdf(path, password, source, error, &reads);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;
    try {
        const int pages = static_cast<int>(QPDFPageDocumentHelper(q).getAllPages().size());
        checkQpdfWarnings(q, reads.get());
        reads->throwIfFailed();
        if (count)
            *count = pages;
        if (encrypted)
            *encrypted = q.isEncrypted();
        if (sourceKey)
            *sourceKey = fileKey(QFileInfo(path));
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
                                     QString *error, int *failedIndex)
{
    if (failedIndex)
        *failedIndex = -1;
    std::unique_ptr<QPDF> source;
    std::shared_ptr<QpdfReadState> reads;
    const Status st = openQpdf(inPath, password, source, error, &reads);
    if (st != Status::Ok) {
        if (failedIndex)
            *failedIndex = 0;
        return st;
    }
    QPDF &q = *source;
    bool reading = true; // an exception names the source in failedIndex
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        const MeasureDoc measurements = measurementPages::read(q);
        checkQpdfWarnings(q, reads.get());
        reading = false;
        const std::set<int> drop(pages.begin(), pages.end());
        const bool shared = sharesResources(q);
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
        removeDeletedContent(q, all, drop, shared);
        MeasureDoc mapped;
        measurementPages::append(mapped, measurements, kept, 0);
        measurementPages::write(q, mapped);
        checkQpdfWarnings(q, reads.get());
        setKeyLength(q);
        AtomicPdfWriter output(q, outPath);
        output.write([&] {
            checkQpdfWarnings(q, reads.get());
            reads->throwIfFailed();
            return true;
        });
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        if (failedIndex && (reading || !reads->error.isEmpty()
                            || qpdfFailurePath(e) == inPath))
            *failedIndex = 0;
        return Status::Failed;
    }
}

PageOps::Status PageOps::rotatePages(const QString &inPath, const QString &outPath,
                                     const QList<int> &pages, int angle, bool relative,
                                     const QString &password, QString *error)
{
    std::unique_ptr<QPDF> source;
    std::shared_ptr<QpdfReadState> reads;
    const Status st = openQpdf(inPath, password, source, error, &reads);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        auto data = measurementPages::read(q);
        checkQpdfWarnings(q, reads.get());
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
        checkQpdfWarnings(q, reads.get());
        setKeyLength(q);
        AtomicPdfWriter output(q, outPath);
        output.write([&] {
            checkQpdfWarnings(q, reads.get());
            reads->throwIfFailed();
            return true;
        });
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
    int reading = -1; // the input being read, which an exception names in failedIndex
    std::vector<std::pair<int, std::shared_ptr<QpdfReadState>>> reads;
    try {
        QPDF out;
        out.emptyPDF();
        QPDFPageDocumentHelper odh(out);
        // Sources must outlive write() so their stream data can be copied. The
        // same file may appear under several paths and contribute several ranges,
        // so each canonical identity is opened exactly once
        // and reused - both to save the parse and because two QPDFs over one file
        // would copy its shared objects into the output twice.
        std::vector<std::unique_ptr<QPDF>> sources;
        std::map<QString, QPDF *> opened;
        std::map<QString, QSet<QString>> passwords;
        SourceForms sourceForms;
        FormCopy forms(out, sourceForms);
        ResourceTrim resources;
        MeasureDoc mergedMeasurements;
        int outputPage = 0;

        for (int i = 0; i < inputs.size(); ++i) {
            reading = i;
            const MergeInput &in = inputs.at(i);
            const QString key = fileKey(QFileInfo(in.path));
            auto it = opened.find(key);
            if (it == opened.end()) {
                std::unique_ptr<QPDF> src;
                std::shared_ptr<QpdfReadState> state;
                const Status st = openQpdf(in.path, in.password, src, error, &state);
                if (st != Status::Ok) {
                    if (failedIndex)
                        *failedIndex = i;
                    return st;
                }
                reads.emplace_back(i, std::move(state));
                it = opened.emplace(key, src.get()).first;
                passwords[key].insert(in.password);
                sources.push_back(std::move(src));
            } else if (!passwords[key].contains(in.password)) {
                // Reusing a handle must not silently accept a conflicting password.
                // Both a valid owner password and a valid user password are allowed.
                std::unique_ptr<QPDF> checked;
                const Status st = openQpdf(in.path, in.password, checked, error);
                if (st != Status::Ok) {
                    if (failedIndex)
                        *failedIndex = i;
                    return st;
                }
                passwords[key].insert(in.password);
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
                resources.before(*it->second, all[static_cast<size_t>(idx)]);
                odh.addPage(all[static_cast<size_t>(idx)], false);
                forms.copied(*it->second, all[static_cast<size_t>(idx)]);
            }
            measurementPages::append(mergedMeasurements, measurementPages::read(*it->second),
                                     selected, outputPage);
            checkQpdfWarnings(*it->second);
            checkQpdfWarnings(out);
            outputPage += selected.size();
        }
        reading = -1;
        forms.finish();
        for (size_t i = 0; i < sources.size(); ++i)
            checkQpdfWarnings(*sources[i], reads[i].second.get());
        checkQpdfWarnings(out);
        measurementPages::write(out, mergedMeasurements);

        AtomicPdfWriter output(out, outPath);
        // Pages of one file keep its encryption. A merge of several files has none.
        if (sources.size() == 1)
            copyEncryption(output, *sources.front());
        output.write([&] {
            for (size_t i = 0; i < sources.size(); ++i) {
                checkQpdfWarnings(*sources[i], reads[i].second.get());
                reads[i].second->throwIfFailed();
            }
            checkQpdfWarnings(out);
            return !mayCommit || mayCommit();
        });
        return Status::Ok;
    } catch (const std::exception &e) {
        if (error)
            *error = QString::fromUtf8(e.what());
        if (failedIndex) {
            *failedIndex = reading;
            const QString path = qpdfFailurePath(e);
            for (const auto &[index, state] : reads)
                if (!state->error.isEmpty() || (!path.isEmpty() && path == state->path)) {
                    *failedIndex = index;
                    break;
                }
        }
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
    splitProjection = {};
    std::unique_ptr<QPDF> source;
    std::shared_ptr<QpdfReadState> reads;
    const Status st = openQpdf(inPath, password, source, error, &reads);
    if (st != Status::Ok)
        return st;
    QPDF &q = *source;
    try {
        QPDFPageDocumentHelper dh(q);
        auto all = dh.getAllPages();
        checkQpdfWarnings(q, reads.get());
        const QDir dir(outDir);
        QStringList paths;
        for (int i = 0; i < static_cast<int>(all.size()); ++i)
            paths << dir.filePath(QStringLiteral("%1-%2.pdf")
                                      .arg(baseName)
                                      .arg(i + 1, 3, 10, QLatin1Char('0')));
        if (mayWrite && !mayWrite(paths))
            return Status::Failed;
        const auto data = measurementPages::read(q);
        checkQpdfWarnings(q, reads.get());
        SourceForms sourceForms;
        ResourceTrim resources;
        SplitFormPages selectedForms(q, all);
        checkQpdfWarnings(q, reads.get());
        for (int i = 0; i < static_cast<int>(all.size()); ++i) {
            QPDF out;
            out.emptyPDF();
            resources.before(q, all[static_cast<size_t>(i)]);
            auto projected = selectedForms.select(all[static_cast<size_t>(i)]);
            SourceForms partForms;
            FormCopy forms(out, projected ? partForms : sourceForms, &sourceForms);
            QPDF &copySource = projected ? *projected : q;
            auto page = projected ? QPDFPageDocumentHelper(*projected).getAllPages().front()
                                 : all[static_cast<size_t>(i)];
            QPDFPageDocumentHelper(out).addPage(page, false);
            forms.copied(copySource, page, &q);
            forms.finish();
            if (projected)
                checkQpdfWarnings(*projected);
            checkQpdfWarnings(q, reads.get());
            checkQpdfWarnings(out);
            MeasureDoc selected;
            measurementPages::append(selected, data, {i}, 0);
            measurementPages::write(out, selected);
            const QString &path = paths.at(i);
            // A stopped job throws here, before it takes the next file.
            AtomicPdfWriter output(out, path);
            copyEncryption(output, q);
            output.write([&] {
                checkQpdfWarnings(q, reads.get());
                if (projected)
                    checkQpdfWarnings(*projected);
                checkQpdfWarnings(out);
                reads->throwIfFailed();
                return !mayCommit || mayCommit(path);
            });
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
