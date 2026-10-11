#pragma once

#include "CoreMinimal.h"
#include "Dom/JsonObject.h"

/**
 * 「用户现在指的是什么」—— 编辑器此刻的焦点和选中状态。
 *
 * ## 存在的理由
 *
 * 用户说「把**这个**节点改成 Lerp」「**这个**材质太亮了」时，句子里没有资产路径，
 * 也没有 node_id。要么模型去猜，要么我们把「这个」是什么如实报上来。
 *
 * ## 只用稳定 API，不遍历 Slate 控件树
 *
 * Epic 的 AIAssistant 有个 SlateQuerier，能顺着 Slate 控件树认出光标下的任何东西
 * （菜单项、按钮、细节面板的某一行）。我们不走那条路，原因是它跟 Slate 的内部控件
 * 结构硬绑，而我们要同时伺候 5.0–5.8 九个引擎版本 —— 控件结构一改，它不会报错，
 * 只会静默答错。
 *
 * 这里只问「选中/焦点」这类有正式 API 的状态：
 *   IAssetEditorInstance::GetLastActivationTime   哪个资产编辑器是真焦点
 *   FBlueprintEditor::GetFocusedGraph/GetSelectedNodes
 *   IMaterialEditor::GetSelectedNodes
 *   GEditor->GetSelectedActors                    关卡里选中的 Actor
 *   IContentBrowserSingleton::GetSelectedAssets   内容浏览器里选中的资产
 * 这几个从 5.0 到 5.8 逐字相同（逐版本查过头文件，不是凭印象）。
 *
 * ## 报出去的 id 必须是调用方能用的
 *
 * 蓝图节点报 NodeGuid（和 blueprint.get_graph 的 node_id 同一套），
 * 材质节点报 FUAL_MaterialCommands::BuildExpressionIds 的编号。
 * 报一套别的工具喂不进去的编号，等于没报。
 */
class FUAL_FocusContext
{
public:
	/**
	 * 拼出完整的焦点上下文 JSON。
	 *
	 * 任何一段查不到都只是少一个字段，不会整体失败 —— 调用方要的是
	 * 「有就用，没有就走别的路径」。
	 */
	static TSharedPtr<FJsonObject> Build();
};
