import * as Y from "yjs";

export type CommentMessage = {
  id: string;
  author: string;
  authorColor: string;
  text: string;
  at: number;
};

export type CommentThread = {
  id: string;
  x: number; // 场景坐标
  y: number;
  author: string;
  authorColor: string;
  createdAt: number;
  resolved: boolean;
  messages: CommentMessage[];
};

function genId(): string {
  return "c_" + Math.random().toString(36).slice(2, 10);
}

/**
 * 评论数据层：所有评论存在 ydoc.getMap("comments")。
 * 每个线程是一个 Y.Map，messages 是 Y.Array（避免整段重写、利于并发回复合并）。
 * 走 Yjs 的好处：零后端改动、自动实时同步、随房间一起持久化。
 */
export class CommentsStore {
  private doc: Y.Doc;
  private yComments: Y.Map<any>;
  private listeners = new Set<() => void>();
  private onChange = () => this.emit();

  constructor(doc: Y.Doc) {
    this.doc = doc;
    this.yComments = doc.getMap("comments");
    this.yComments.observeDeep(this.onChange);
  }

  destroy() {
    this.yComments.unobserveDeep(this.onChange);
    this.listeners.clear();
  }

  private emit() {
    this.listeners.forEach((l) => l());
  }

  subscribe(cb: () => void): () => void {
    this.listeners.add(cb);
    return () => {
      this.listeners.delete(cb);
    };
  }

  getAll(): CommentThread[] {
    const out: CommentThread[] = [];
    this.yComments.forEach((v: any, id: string) => {
      if (v && typeof v.get === "function") out.push(this.toThread(id, v));
    });
    return out.sort((a, b) => a.createdAt - b.createdAt);
  }

  private toThread(id: string, m: any): CommentThread {
    const raw = m.get("messages");
    const arr = raw && typeof raw.toArray === "function" ? raw.toArray() : [];
    const messages = arr.map((mm: any) => (typeof mm.toJSON === "function" ? mm.toJSON() : mm));
    return {
      id,
      x: m.get("x"),
      y: m.get("y"),
      author: m.get("author"),
      authorColor: m.get("authorColor"),
      createdAt: m.get("createdAt"),
      resolved: !!m.get("resolved"),
      messages,
    };
  }

  /** 在场景坐标 (x,y) 处新建一条评论线程（首条消息由调用方通过 addMessage 补） */
  addThread(x: number, y: number, author: string, authorColor: string): string {
    const id = genId();
    this.doc.transact(() => {
      const m = new Y.Map();
      m.set("x", x);
      m.set("y", y);
      m.set("author", author);
      m.set("authorColor", authorColor);
      m.set("createdAt", Date.now());
      m.set("resolved", false);
      const msgs = new Y.Array();
      m.set("messages", msgs);
      this.yComments.set(id, m);
    });
    return id;
  }

  addMessage(threadId: string, author: string, authorColor: string, text: string) {
    const m = this.yComments.get(threadId);
    if (!m) return;
    this.doc.transact(() => {
      const msgs = m.get("messages") as Y.Array<any>;
      msgs.push([{ id: genId(), author, authorColor, text, at: Date.now() }]);
    });
  }

  toggleResolved(threadId: string) {
    const m = this.yComments.get(threadId);
    if (!m) return;
    this.doc.transact(() => m.set("resolved", !m.get("resolved")));
  }

  removeThread(threadId: string) {
    this.doc.transact(() => {
      this.yComments.delete(threadId);
    });
  }
}
