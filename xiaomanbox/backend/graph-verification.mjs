/** Read the exported plugin graph contract: node_id, category, pin.linked_to. */
const kind = p => String(p.category ?? p.type ?? '').toLowerCase();
const dataPin = p => kind(p) !== 'exec' && !kind(p).startsWith('delegate');
const nodeKey = n => n.node_id ?? n.id;
export function graphReadbackArgs(submitted) {
  return {blueprint_path: submitted.blueprint_path, ...(submitted.graph_name ? {graph_name: submitted.graph_name} : {})};
}
export function verifyGraph(graph, submitted, created = []) {
  const nodes = graph?.nodes || [];
  const ids = new Map(created.filter(n => n.id && n.node_id).map(n => [n.id, n.node_id]));
  const explicitDefaults = new Set();
  for (const n of submitted?.nodes || []) {
    for (const key of Object.keys(n.pin_defaults || {})) explicitDefaults.add(`${ids.get(n.id) ?? n.id}.${key}`);
  }
  const connected = new Set(), used = new Set();
  const byId = new Map(nodes.map(n => [nodeKey(n), n]));
  for (const n of nodes) for (const p of n.pins || []) {
    const links = Array.isArray(p.linked_to) ? p.linked_to : [];
    if (p.dir === 'Input' && links.length) connected.add(`${nodeKey(n)}.${p.name}`);
    if (p.dir === 'Output' && dataPin(p) && links.length) used.add(nodeKey(n));
  }
  // Retain support for explicit connections on compatible external graph responses.
  for (const c of graph?.connections || []) {
    connected.add(`${c.to_node}.${c.to_pin}`);
    const p = byId.get(c.from_node)?.pins?.find(p => p.name === c.from_pin);
    if (p?.dir === 'Output' && dataPin(p)) used.add(c.from_node);
  }
  const issues = [];
  for (const n of nodes) {
    const pins = n.pins || [], id = nodeKey(n);
    if (!id || !used.has(id) || pins.some(p => p.dir === 'Input' && kind(p) === 'exec')) continue;
    if (!pins.some(p => p.dir === 'Output' && dataPin(p))) continue;
    for (const p of pins) {
      if (p.dir !== 'Input' || !dataPin(p) || ['self','target','__worldcontext','worldcontextobject'].includes(p.name.toLowerCase())) continue;
      const key = `${id}.${p.name}`;
      if (!connected.has(key) && !explicitDefaults.has(key)) issues.push({node: n.title || n.member_name || n.class, node_id: id, pin: p.name});
    }
  }
  return issues;
}
