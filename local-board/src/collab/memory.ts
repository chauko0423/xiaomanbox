import * as Y from "yjs";

/** Clear expired entries through Yjs, preserving protection for entries still in use. */
export function trimUndoHistory(doc: Y.Doc, manager: Y.UndoManager, maxSteps: number) {
  const expired = manager.undoStack.slice(0, -maxSteps);
  if (!expired.length) return;
  const retained = manager.undoStack.slice(-maxSteps);
  doc.transact(tr => {
    manager.undoStack = expired;
    manager.clear(true, false);
    manager.undoStack = retained;
    for (const entry of [...retained, ...manager.redoStack]) {
      Y.iterateDeletedStructs(tr, entry.deletions, (item: any) => {
        if (!(item instanceof Y.Item)) return;
        for (let current: any = item; current; current = current.parent?._item) current.keep = true;
      });
    }
    for (const entry of expired) Y.tryGc(entry.deletions, doc.store, doc.gcFilter);
  }, "history-maintenance");
}

export function undoFileIds(doc: Y.Doc, manager: Y.UndoManager | null): Set<string> {
  const ids = new Set<string>();
  if (!manager) return ids;
  doc.transact(tr => {
    for (const entry of [...manager.undoStack, ...manager.redoStack]) {
      for (const ds of [entry.deletions, entry.insertions]) {
        Y.iterateDeletedStructs(tr, ds, (item: any) => {
          if (!(item instanceof Y.Item)) return;
          if (item.parentSub === "fileId") {
            for (const value of item.content.getContent()) if (typeof value === "string") ids.add(value);
          }
          for (const value of item.content.getContent()) {
            if (value instanceof Y.Map) {
              const id = value.get("fileId");
              if (typeof id === "string") ids.add(id);
            }
          }
        });
      }
    }
  }, "history-inspection");
  return ids;
}
