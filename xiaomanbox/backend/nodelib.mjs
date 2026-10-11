// The external NodeUE template dataset is excluded pending redistribution permission.
export const nodeLibStats=()=>({available:false,count:0,total:0,parents:[],note:'基础版不附带第三方节点模板数据；可使用 UE 在线工具查询。'});
export const nodeSearch=()=>({ok:false,error:'节点模板数据未随基础版提供'});
export const nodeDescribe=nodeSearch;
export const nodeList=()=>({ok:true,nodes:[]});
