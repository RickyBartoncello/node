// Copyright 2018 the V8 project authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef V8_OBJECTS_JS_WEAK_REFS_INL_H_
#define V8_OBJECTS_JS_WEAK_REFS_INL_H_

#include "src/objects/js-weak-refs.h"

#include "src/api/api-inl.h"
#include "src/heap/heap-write-barrier-inl.h"
#include "src/objects/smi-inl.h"

// Has to be the last include (doesn't have include guards):
#include "src/objects/object-macros.h"

namespace v8 {
namespace internal {

TQ_OBJECT_CONSTRUCTORS_IMPL(WeakCell)
TQ_OBJECT_CONSTRUCTORS_IMPL(JSWeakRef)
OBJECT_CONSTRUCTORS_IMPL(JSFinalizationGroup, JSObject)
TQ_OBJECT_CONSTRUCTORS_IMPL(JSFinalizationGroupCleanupIterator)

ACCESSORS(JSFinalizationGroup, native_context, NativeContext,
          kNativeContextOffset)
ACCESSORS(JSFinalizationGroup, cleanup, Object, kCleanupOffset)
ACCESSORS(JSFinalizationGroup, active_cells, HeapObject, kActiveCellsOffset)
ACCESSORS(JSFinalizationGroup, cleared_cells, HeapObject, kClearedCellsOffset)
ACCESSORS(JSFinalizationGroup, key_map, Object, kKeyMapOffset)
SMI_ACCESSORS(JSFinalizationGroup, flags, kFlagsOffset)
ACCESSORS(JSFinalizationGroup, next, Object, kNextOffset)
CAST_ACCESSOR(JSFinalizationGroup)

void JSFinalizationGroup::Register(
    Handle<JSFinalizationGroup> finalization_group, Handle<JSReceiver> target,
    Handle<Object> holdings, Handle<Object> unregister_token,
    Isolate* isolate) {
  Handle<WeakCell> weak_cell = isolate->factory()->NewWeakCell();
  weak_cell->set_finalization_group(*finalization_group);
  weak_cell->set_target(*target);
  weak_cell->set_holdings(*holdings);
  weak_cell->set_prev(ReadOnlyRoots(isolate).undefined_value());
  weak_cell->set_next(ReadOnlyRoots(isolate).undefined_value());
  weak_cell->set_unregister_token(*unregister_token);
  weak_cell->set_key_list_prev(ReadOnlyRoots(isolate).undefined_value());
  weak_cell->set_key_list_next(ReadOnlyRoots(isolate).undefined_value());

  // Add to active_cells.
  weak_cell->set_next(finalization_group->active_cells());
  if (finalization_group->active_cells().IsWeakCell()) {
    WeakCell::cast(finalization_group->active_cells()).set_prev(*weak_cell);
  }
  finalization_group->set_active_cells(*weak_cell);

  if (!unregister_token->IsUndefined(isolate)) {
    Handle<SimpleNumberDictionary> key_map;
    if (finalization_group->key_map().IsUndefined(isolate)) {
      key_map = SimpleNumberDictionary::New(isolate, 1);
    } else {
      key_map = handle(
          SimpleNumberDictionary::cast(finalization_group->key_map()), isolate);
    }

    // Unregister tokens are held weakly as objects are often their own
    // unregister token. To avoid using an ephemeron map, the map for token
    // lookup is keyed on the token's identity hash instead of the token itself.
    uint32_t key = unregister_token->GetOrCreateHash(isolate).value();
    InternalIndex entry = key_map->FindEntry(isolate, key);
    if (entry.is_found()) {
      Object value = key_map->ValueAt(entry);
      WeakCell existing_weak_cell = WeakCell::cast(value);
      existing_weak_cell.set_key_list_prev(*weak_cell);
      weak_cell->set_key_list_next(existing_weak_cell);
    }
    key_map = SimpleNumberDictionary::Set(isolate, key_map, key, weak_cell);
    finalization_group->set_key_map(*key_map);
  }
}

bool JSFinalizationGroup::Unregister(
    Handle<JSFinalizationGroup> finalization_group,
    Handle<JSReceiver> unregister_token, Isolate* isolate) {
  // Iterate through the doubly linked list of WeakCells associated with the
  // key. Each WeakCell will be in the "active_cells" or "cleared_cells" list of
  // its FinalizationGroup; remove it from there.
  return finalization_group->RemoveUnregisterToken(
      *unregister_token, isolate,
      [isolate](WeakCell matched_cell) {
        matched_cell.RemoveFromFinalizationGroupCells(isolate);
      },
      [](HeapObject, ObjectSlot, Object) {});
}

template <typename MatchCallback, typename GCNotifyUpdatedSlotCallback>
bool JSFinalizationGroup::RemoveUnregisterToken(
    JSReceiver unregister_token, Isolate* isolate, MatchCallback match_callback,
    GCNotifyUpdatedSlotCallback gc_notify_updated_slot) {
  // This method is called from both FinalizationGroup#unregister and for
  // removing weakly-held dead unregister tokens. The latter is during GC so
  // this function cannot GC.
  DisallowHeapAllocation no_gc;
  if (key_map().IsUndefined(isolate)) {
    return false;
  }

  SimpleNumberDictionary key_map =
      SimpleNumberDictionary::cast(this->key_map());
  // If the token doesn't have a hash, it was not used as a key inside any hash
  // tables.
  Object hash = unregister_token.GetHash();
  if (hash.IsUndefined(isolate)) {
    return false;
  }
  uint32_t key = Smi::ToInt(hash);
  InternalIndex entry = key_map.FindEntry(isolate, key);
  if (entry.is_not_found()) {
    return false;
  }

  Object value = key_map.ValueAt(entry);
  bool was_present = false;
  HeapObject undefined = ReadOnlyRoots(isolate).undefined_value();
  HeapObject new_key_list_head = undefined;
  HeapObject new_key_list_prev = undefined;
  // Compute a new key list that doesn't have unregister_token. Because
  // unregister tokens are held weakly, key_map is keyed using the tokens'
  // identity hashes, and identity hashes may collide.
  while (!value.IsUndefined(isolate)) {
    WeakCell weak_cell = WeakCell::cast(value);
    DCHECK(!ObjectInYoungGeneration(weak_cell));
    value = weak_cell.key_list_next();
    if (weak_cell.unregister_token() == unregister_token) {
      // weak_cell has the same unregister token; remove it from the key list.
      match_callback(weak_cell);
      weak_cell.set_key_list_prev(undefined);
      weak_cell.set_key_list_next(undefined);
      was_present = true;
    } else {
      // weak_cell has a different unregister token with the same key (hash
      // collision); fix up the list.
      weak_cell.set_key_list_prev(new_key_list_prev);
      gc_notify_updated_slot(weak_cell,
                             weak_cell.RawField(WeakCell::kKeyListPrevOffset),
                             new_key_list_prev);
      weak_cell.set_key_list_next(undefined);
      if (new_key_list_prev.IsUndefined(isolate)) {
        new_key_list_head = weak_cell;
      } else {
        DCHECK(new_key_list_head.IsWeakCell());
        WeakCell prev_cell = WeakCell::cast(new_key_list_prev);
        prev_cell.set_key_list_next(weak_cell);
        gc_notify_updated_slot(prev_cell,
                               prev_cell.RawField(WeakCell::kKeyListNextOffset),
                               weak_cell);
      }
      new_key_list_prev = weak_cell;
    }
  }
  if (new_key_list_head.IsUndefined(isolate)) {
    DCHECK(was_present);
    key_map.ClearEntry(isolate, entry);
    key_map.ElementRemoved();
  } else {
    key_map.ValueAtPut(entry, new_key_list_head);
    gc_notify_updated_slot(key_map, key_map.RawFieldOfValueAt(entry),
                           new_key_list_head);
  }
  return was_present;
}

bool JSFinalizationGroup::NeedsCleanup() const {
  return cleared_cells().IsWeakCell();
}

bool JSFinalizationGroup::scheduled_for_cleanup() const {
  return ScheduledForCleanupField::decode(flags());
}

void JSFinalizationGroup::set_scheduled_for_cleanup(
    bool scheduled_for_cleanup) {
  set_flags(ScheduledForCleanupField::update(flags(), scheduled_for_cleanup));
}

Object JSFinalizationGroup::PopClearedCellHoldings(
    Handle<JSFinalizationGroup> finalization_group, Isolate* isolate) {
  Handle<WeakCell> weak_cell =
      handle(WeakCell::cast(finalization_group->cleared_cells()), isolate);
  DCHECK(weak_cell->prev().IsUndefined(isolate));
  finalization_group->set_cleared_cells(weak_cell->next());
  weak_cell->set_next(ReadOnlyRoots(isolate).undefined_value());

  if (finalization_group->cleared_cells().IsWeakCell()) {
    WeakCell cleared_cells_head =
        WeakCell::cast(finalization_group->cleared_cells());
    DCHECK_EQ(cleared_cells_head.prev(), *weak_cell);
    cleared_cells_head.set_prev(ReadOnlyRoots(isolate).undefined_value());
  } else {
    DCHECK(finalization_group->cleared_cells().IsUndefined(isolate));
  }

  // Also remove the WeakCell from the key_map (if it's there).
  if (!weak_cell->unregister_token().IsUndefined(isolate)) {
    if (weak_cell->key_list_prev().IsUndefined(isolate)) {
      Handle<SimpleNumberDictionary> key_map = handle(
          SimpleNumberDictionary::cast(finalization_group->key_map()), isolate);
      Handle<Object> unregister_token =
          handle(weak_cell->unregister_token(), isolate);
      uint32_t key = Smi::ToInt(unregister_token->GetHash());
      InternalIndex entry = key_map->FindEntry(isolate, key);

      if (weak_cell->key_list_next().IsUndefined(isolate)) {
        // weak_cell is the only one associated with its key; remove the key
        // from the hash table.
        DCHECK(entry.is_found());
        key_map = SimpleNumberDictionary::DeleteEntry(isolate, key_map, entry);
        finalization_group->set_key_map(*key_map);
      } else {
        // weak_cell is the list head for its key; we need to change the value
        // of the key in the hash table.
        Handle<WeakCell> next =
            handle(WeakCell::cast(weak_cell->key_list_next()), isolate);
        DCHECK_EQ(next->key_list_prev(), *weak_cell);
        next->set_key_list_prev(ReadOnlyRoots(isolate).undefined_value());
        weak_cell->set_key_list_next(ReadOnlyRoots(isolate).undefined_value());
        key_map = SimpleNumberDictionary::Set(isolate, key_map, key, next);
        finalization_group->set_key_map(*key_map);
      }
    } else {
      // weak_cell is somewhere in the middle of its key list.
      WeakCell prev = WeakCell::cast(weak_cell->key_list_prev());
      prev.set_key_list_next(weak_cell->key_list_next());
      if (!weak_cell->key_list_next().IsUndefined()) {
        WeakCell next = WeakCell::cast(weak_cell->key_list_next());
        next.set_key_list_prev(weak_cell->key_list_prev());
      }
    }
  }

  return weak_cell->holdings();
}

template <typename GCNotifyUpdatedSlotCallback>
void WeakCell::Nullify(Isolate* isolate,
                       GCNotifyUpdatedSlotCallback gc_notify_updated_slot) {
  // Remove from the WeakCell from the "active_cells" list of its
  // JSFinalizationGroup and insert it into the "cleared_cells" list. This is
  // only called for WeakCells which haven't been unregistered yet, so they will
  // be in the active_cells list. (The caller must guard against calling this
  // for unregistered WeakCells by checking that the target is not undefined.)
  DCHECK(target().IsJSReceiver());
  set_target(ReadOnlyRoots(isolate).undefined_value());

  JSFinalizationGroup fg = JSFinalizationGroup::cast(finalization_group());
  if (prev().IsWeakCell()) {
    DCHECK_NE(fg.active_cells(), *this);
    WeakCell prev_cell = WeakCell::cast(prev());
    prev_cell.set_next(next());
    gc_notify_updated_slot(prev_cell, prev_cell.RawField(WeakCell::kNextOffset),
                           next());
  } else {
    DCHECK_EQ(fg.active_cells(), *this);
    fg.set_active_cells(next());
    gc_notify_updated_slot(
        fg, fg.RawField(JSFinalizationGroup::kActiveCellsOffset), next());
  }
  if (next().IsWeakCell()) {
    WeakCell next_cell = WeakCell::cast(next());
    next_cell.set_prev(prev());
    gc_notify_updated_slot(next_cell, next_cell.RawField(WeakCell::kPrevOffset),
                           prev());
  }

  set_prev(ReadOnlyRoots(isolate).undefined_value());
  Object cleared_head = fg.cleared_cells();
  if (cleared_head.IsWeakCell()) {
    WeakCell cleared_head_cell = WeakCell::cast(cleared_head);
    cleared_head_cell.set_prev(*this);
    gc_notify_updated_slot(cleared_head_cell,
                           cleared_head_cell.RawField(WeakCell::kPrevOffset),
                           *this);
  }
  set_next(fg.cleared_cells());
  gc_notify_updated_slot(*this, RawField(WeakCell::kNextOffset), next());
  fg.set_cleared_cells(*this);
  gc_notify_updated_slot(
      fg, fg.RawField(JSFinalizationGroup::kClearedCellsOffset), *this);
}

void WeakCell::RemoveFromFinalizationGroupCells(Isolate* isolate) {
  // Remove the WeakCell from the list it's in (either "active_cells" or
  // "cleared_cells" of its JSFinalizationGroup).

  // It's important to set_target to undefined here. This guards that we won't
  // call Nullify (which assumes that the WeakCell is in active_cells).
  DCHECK(target().IsUndefined() || target().IsJSReceiver());
  set_target(ReadOnlyRoots(isolate).undefined_value());

  JSFinalizationGroup fg = JSFinalizationGroup::cast(finalization_group());
  if (fg.active_cells() == *this) {
    DCHECK(prev().IsUndefined(isolate));
    fg.set_active_cells(next());
  } else if (fg.cleared_cells() == *this) {
    DCHECK(!prev().IsWeakCell());
    fg.set_cleared_cells(next());
  } else {
    DCHECK(prev().IsWeakCell());
    WeakCell prev_cell = WeakCell::cast(prev());
    prev_cell.set_next(next());
  }
  if (next().IsWeakCell()) {
    WeakCell next_cell = WeakCell::cast(next());
    next_cell.set_prev(prev());
  }
  set_prev(ReadOnlyRoots(isolate).undefined_value());
  set_next(ReadOnlyRoots(isolate).undefined_value());
}

}  // namespace internal
}  // namespace v8

#include "src/objects/object-macros-undef.h"

#endif  // V8_OBJECTS_JS_WEAK_REFS_INL_H_
