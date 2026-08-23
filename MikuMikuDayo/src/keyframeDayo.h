//キーフレーム編集ウィンドウ

#pragma once
#include "defsDayo.h"


//キーの表示状態、キーなし、選択されているキー、選択されていないキー
//bit0:有無、bit1:選択、bit2:×(物理on)
enum class KeyDisp {none = 0, has = 1, select = 2, cross = 4};
KeyDisp operator|(KeyDisp L, KeyDisp R) { return static_cast<KeyDisp>(static_cast<uint64_t>(L) | static_cast<uint64_t>(R)); }
KeyDisp operator&(KeyDisp L, KeyDisp R) { return static_cast<KeyDisp>(static_cast<uint64_t>(L) & static_cast<uint64_t>(R)); }

typedef std::variant<
	std::map<int, PMX::VMDMotionLite>*,
	std::map<int, PMX::VMDMorphLite>*,
	std::map<int, PMX::VMDExtra>*,
	std::map<int, PMX::VMDCamera>*,
	std::map<int, PMX::VMDLight>*,
	std::map<int, PMX::VMDSelfShadow>*,
	std::map<int, PMX::VMDGravity>*
> KeysVariant;

//表示枠の▼をクリックすると出てくる子要素
struct KeyFrameLeaf {
	//編集対象の変更が無い限りは変わらないメンバ
	KeysVariant source;		//solverの<int,map>型のキーフレーム列
	std::string name;		//utf-8に変換済みの名前
	//以下は毎フレーム書き換わるメンバ
	bool selected = false;	//UIから選択されているか？
	int dispRow = -1;	//表示されている場合、テーブルの何行目に表示されているか？たたまれている場合は親ノードの行番号
	int index = -1;		//ボーン番号orモーフ番号, それ以外のための枠だった場合-1
	std::vector<KeyDisp>keydisp;	//表示されている範囲でのキーの有無
};

//キーフレーム表示窓の表示枠1つについての情報
struct KeyFrameNode {
	bool direct = false;//ツリーノードを使わずリーフをセルに直接表示するフラグ
	std::string name;	//UTF-8に変換された表示枠名
	std::vector<KeyFrameLeaf>leaves;
	std::vector<KeyDisp>keydisp;	//表示されている範囲でのキーの有無(リーフのorを取った値)
	//選択状態を変更し、子ノードにも伝搬する
	void Select(bool state = true) {
		for (auto&& c : leaves)
			c.selected = state;
	}
	//UIから選択されているか？(子ノードが全て選択状態だった時のみtrue)
	bool Selected() {
		if (leaves.empty())
			return false;
		for (auto&& c : leaves)
			if (!c.selected)
				return false;
		return true;
	}
};

//コピペのためのバッファ
struct KeyFrameCopyBuffer {
	PMX::KeySubset subset;
};

//アンドゥ・リドゥのための情報
struct KeyUndoRedo {
	int modelID = 0;			//モデルID
	int pack = 1;				//何個の操作の組みか？2つ以上の操作の組みをまとめてアンドゥ・リドゥできるようにするための情報
	std::string target;			//操作対象を示す文字列(utf-8)
	std::string description;	//操作内容を示す文字列
	std::string time;			//操作時刻を示す文字列
	PMX::KeyUndo undo, redo;
};


//アンドゥのためのバッファ
struct KeyFrameUndoBuffer {
	int maxUndo = 32;	//最大アンドゥ回数

	//リドゥ用インデックス
	//最後のアンドゥ時に使われたundos上のインデックスを指す(リドゥや新たな操作なしで連続してアンドゥした回数)
	//アンドゥが1回される毎に+1され、リドゥ1回ごとに-1される
	//index >= buffer.size()の時アンドゥ不可。index == 0の時、リドゥ不可
	//undo情報の格納されたモデル番号とアンドゥ情報のペアとして保持される
	int index = 0;
	std::vector<KeyUndoRedo> buffer;

	//アンドゥ・リドゥ情報をプッシュする
	//pack : Ctrl+Z,Yを押したときに幾つの操作を一度ににアンドゥするか？
	void Push(int id, const PMX::KeyUndo& undo, const PMX::KeyUndo& redo, const std::string& u8target, const std::wstring & description = L"", int pack = 1) {

		//これから何かフレームに対するアンドゥ可能な操作がなされるのでリドゥ用のバッファを切断する
		for (int i = 0; i < index; i++) {
			buffer.erase(buffer.begin());
		}
		index = 0;

		char timestr[80];
		auto tm = time(nullptr);
		auto timeptr = localtime(&tm);
		strftime(timestr, 80, "%R:%S", timeptr);
		buffer.insert(buffer.begin(), { id, pack, u8target, YRZ::wstrToUTF8(description), timestr, undo, redo });
		if (buffer.size() > maxUndo) {
			buffer.erase(buffer.begin() + buffer.size() - 1);
		}
	}

	//モデルが削除された時用。idに合致するアンドゥ情報の削除
	void Purge(int id) {
		int n = buffer.size();
		for (int i = n-1; i >= 0; i--) {
			if (buffer[i].modelID == id) {
				buffer.erase(buffer.begin() + i);
				if (i <= index)
					index = max(0, index - 1);
			}
		}
		//次に、アンドゥバッファ内の該当IDについての外部親キーの削除
		for (auto&& u : buffer) {
			for (auto&& ex : u.redo.rev.extras)
				std::erase_if(ex.exp, [&](PMX::VMDExParent& o) { return o.parentID == id; });
			for (auto&& ex : u.undo.rev.extras)
				std::erase_if(ex.exp, [&](PMX::VMDExParent& o) { return o.parentID == id; });
		}
	}

	bool Undoable() { return index < buffer.size(); }
	bool Redoable() { return index > 0; }
};

//キーフレームの変更についての状態
// 現在のフレームが開始されてから…
// none : 何も変わってない
// frame : カレントフレームの移動
// check : チェック状態の変更
// modify : キーの追加・削除・登録が行われた
// reset : キーフレームコンテキストのリセットが行われた
enum class KeyFrameStatus { none = 0, frame=1, check=2, modify=4, reset=8 };
KeyFrameStatus operator|(KeyFrameStatus L, KeyFrameStatus R) { return static_cast<KeyFrameStatus>(static_cast<uint64_t>(L) | static_cast<uint64_t>(R)); }
KeyFrameStatus operator&(KeyFrameStatus L, KeyFrameStatus R) { return static_cast<KeyFrameStatus>(static_cast<uint64_t>(L) & static_cast<uint64_t>(R)); }


//キーフレーム表示窓を表示・操作するために保持が必要な情報
struct KeyFrameWindowContext {
	int id;				//アンドゥバッファに登録する際のID
	PMX::PMXModel* pmx;	//編集対象のモデルのボーン名などを得るためのデータ。nullptrの場合はカメラ編集
	PMX::ISolver* solver;
	KeyFrameUndoBuffer* undoBuffer;	//アンドゥ・リドゥのためのバッファ
	KeyFrameCopyBuffer* copyBuffer;	//コピペのためのバッファ
	std::string name;	//操作対象の名前(utf-8)
	std::wstring namew;	//操作対象の名前(wstring)

	int maxLeaves = 0;		//ノードのうち最も多くリーフを持っているノードのリーフ数
	PMX::Pose pose;			//編集中のポーズ(モデル編集用の場合だけ持っている)

	std::vector<KeyFrameNode>nodes;	//表示枠名(utf-8)
	//コンテクストをリセットする(編集対象のモデルが切り替わった時に呼ぶ)

	//ドラッグ情報格納用
	bool dragging = false;	//左ドラッグによる範囲セル選択中か？
	int dragStartX = -1, dragStartY = -1, dragEndX = -1, dragEndY = -1;	//ドラッグ開始・ドラッグ終了(or継続中の最後)時にマウスが乗っているセルの列・行
	bool dragFinishing = false;	//「今ドラッグが終わったので選択状態の更新してね」フラグ

	//ボーンのShift+クリック用
	int prevClickedNode = -1;	//最後にテーブルの何行目に表示されているノードまたはリーフをクリックしたか？
	int prevClickedLeaf = -1;
	
	KeyFrameStatus status = KeyFrameStatus::none;	//キーフレームの有無リストがアップデートされた
	std::unordered_set<int>registeredBones;		//ボーン表示枠に登録されているボーンの番号リスト

	//その他情報
	double wavOffset = 0;	//音声波形表示時のオフセット[秒]

	//キーフレームの有無リストを更新する
	void UpdateKeyDisp(KeyFrameStatus reason) {
		status = status | reason;

		int iFrameLeft = max(0, Frame - nKeys / 2);

		for (int i = 0; auto && node:nodes) {
			fill(node.keydisp.begin(), node.keydisp.end(), KeyDisp::none);
			//リーフの表示状態の更新
			for (int j = 0; auto && leaf:node.leaves) {
				for (int x = 0, found = 0; x < nKeys; x++) {
					bool haskey, sel = false;
					int f = iFrameLeft + x;
					std::visit([&](auto& o) { haskey = o->contains(f); if (haskey) { sel = o->at(f).selected;}}, leaf.source);
					if (haskey) {
						leaf.keydisp[x] = sel ? KeyDisp::select|KeyDisp::has : KeyDisp::has;
						//物理ボーンについてのリーフだったら物理の状態も入れる
						if (leaf.source.index() == 0) {
							auto o = std::get<0>(leaf.source);
							if (pmx->bones[leaf.index].IsPhysics && o->at(f).physics)
								leaf.keydisp[x] = leaf.keydisp[x] | KeyDisp::cross;
						}
						found++;
					} else
						leaf.keydisp[x] = KeyDisp::none;
				}
				j++;
			}
			//リーフの表示状態のORを取ってノードの表示状態とする
			for (int x = 0; x < nKeys; x++) {
				for (auto&& leaf : node.leaves)
					node.keydisp[x] = (KeyDisp)((UINT)node.keydisp[x] | (UINT)leaf.keydisp[x]);// &~(UINT)KeyDisp::cross);
			}
			i++;
		}
	}

	//モデルの切り替えやsolverに格納されたキーの内容が更新された時に読み直す
	//keepNodeSelectionがtrueの時、キーフレーム表示の更新用とみなし、ノード・リーフの選択状態はキープされる
	void Reset(PMX::PMXModel* _pmx, PMX::ISolver* _solver, KeyFrameCopyBuffer* _copybuf, KeyFrameUndoBuffer* _undobuf, bool keepNodeSelection = false) {
		pmx = _pmx;
		copyBuffer = _copybuf;
		undoBuffer = _undobuf;
		maxLeaves = 0;
		solver = _solver;
		registeredBones.clear();
		prevClickedLeaf = prevClickedNode = -1;

		if (pmx != nullptr) {
			namew = pmx->name;
			name = YRZ::wstrToUTF8(namew);

			//モデル用
			auto solver = (PMX::PoseSolver*)_solver;

			/* 表示枠 */
			nodes.resize(pmx->nodes.size() + 1);	//表示・IK・外親で+1
			//表示・IK・外親枠
			nodes[0].direct = true;
			nodes[0].name = g_hon.Do("disp/IK/external");
			nodes[0].keydisp.resize(nKeys);
			nodes[0].leaves.resize(1);
			//表示
			nodes[0].leaves[0].name = nodes[0].name;
			nodes[0].leaves[0].keydisp.resize(nKeys);
			nodes[0].leaves[0].source = &solver->extraKeys;
			if (!keepNodeSelection)
				nodes[0].leaves[0].selected = false;
			maxLeaves = 1;

			//ボーン・表情
			for (int i = 1; auto && n : pmx->nodes) {
				nodes[i].direct = (n.flag == 1) && (n.name == L"Root");
				nodes[i].leaves.resize(n.items.size());
				//日本語モードの時はノード名は日本語、それ以外では英語優先で引っ張ってくる
				if (Language == 0 || n.name_e.empty())
					nodes[i].name = YRZ::wstrToUTF8(n.name);
				else
					nodes[i].name = YRZ::wstrToUTF8(n.name_e);
				nodes[i].keydisp.resize(nKeys);
				maxLeaves = max((size_t)maxLeaves, n.items.size());
				for (int j = 0; auto && item:n.items) {
					auto& leaf = nodes[i].leaves[j];
					leaf.index = item.index;
					if (item.isBone) {
						leaf.source = &solver->motionKeys[item.index];
						auto& pb = pmx->bones[item.index];
						if (Language == 0 || pb.name_e.empty())
							leaf.name = YRZ::wstrToUTF8(pb.name);
						else
							leaf.name = YRZ::wstrToUTF8(pb.name_e);
						registeredBones.insert(item.index);
					} else {
						leaf.source = &solver->morphKeys[item.index];
						auto& pm = pmx->morphs[item.index];
						if (Language == 0 || pm.name_e.empty())
							leaf.name = YRZ::wstrToUTF8(pm.name);
						else
							leaf.name = YRZ::wstrToUTF8(pm.name_e);
					}
					if (!keepNodeSelection)
						leaf.selected = false;
					leaf.keydisp.resize(nKeys);
					j++;
				}
				i++;
			}
			//0番のボーン(大概は「全ての親」)を選択状態にする
			if (!keepNodeSelection) {
				SelectBoneNode(0);
			}
		} else {
			//カメラ用
			namew = L"Camera";
			name = YRZ::wstrToUTF8(namew);

			auto lambda = [&](KeyFrameNode& node, auto name, KeysVariant source) {
				node.direct = true;
				node.name = name;
				node.keydisp.resize(nKeys);
				node.leaves.resize(1);
				node.leaves[0].name = name;
				node.leaves[0].keydisp.resize(nKeys);
				node.leaves[0].source = source;
				if (!keepNodeSelection)
					node.leaves[0].selected = false;
				};

			auto solver = (PMX::CameraSolver*)_solver;
			nodes.resize(4);
			lambda(nodes[0], g_hon.Do("camera"), &solver->cameraKeys);
			lambda(nodes[1], g_hon.Do("light"), &solver->lightKeys);
			lambda(nodes[2], g_hon.Do("s shadow"), &solver->selfShadowKeys);
			lambda(nodes[3], g_hon.Do("gravity"), &solver->gravityKeys);
			maxLeaves = 1;
		}
		UpdateKeyDisp(KeyFrameStatus::reset);
	}

	void Reset()
	{
		Reset(pmx, solver, copyBuffer, undoBuffer, true);
	}

	//ボーン・表情とも全部選択/非選択状態にする
	void SelectAll(bool state) {
		for (auto&& n : nodes)
			n.Select(state);
	}

	//キーフレームを全部非選択にする(UpdateKeyDispは呼び出しもとで呼んでね)
	void SelectAllKeys(bool select) {
		if (solver->type == PMX::SolverType::pose) {
			auto psolver = (PMX::PoseSolver*)solver;
			for (auto&& m : psolver->motionKeys)
				for (auto&& key : m)
					key.second.selected = select;

			for (auto&& m : psolver->morphKeys)
				for (auto&& key : m)
					key.second.selected = select;

			for (auto&& key : psolver->extraKeys)
				key.second.selected = select;
		} else {
			auto csolver = (PMX::CameraSolver*)solver;
			for (auto&& key : csolver->cameraKeys)
				key.second.selected = select;
			for (auto&& key : csolver->lightKeys)
				key.second.selected = select;
			for (auto&& key : csolver->selfShadowKeys)
				key.second.selected = select;
			for (auto&& key : csolver->gravityKeys)
				key.second.selected = select;
		}
	}

	//セレクタブルのID
	//何番のノードの何番のリーフの左から何番目のセレクタブルとして配置される物か、という事からidを作る
	//iLeaf == maxLeavesの時は閉じたノードに配置されているセレクタブルを表している
	int SelectableID(int iNode, int iLeaf, int iSelectable) {
		return iNode * ((maxLeaves + 1) * nKeys) + iLeaf * nKeys + iSelectable;
	}
	//idからノード・リーフ・セレクタブル番号を逆算する。返り値がtrueの時は閉じたノード上のセレクタブルを表す
	bool DecodeSelectableID(int id, int& iNode, int& iLeaf, int& iSelectable) {
		iSelectable = id % nKeys;
		int nodeleaf = id / nKeys;
		iLeaf = nodeleaf % (maxLeaves + 1);
		iNode = nodeleaf / (maxLeaves + 1);
		return iLeaf == maxLeaves;
	}

	//col,rowで示されるセルはドラッグ中の矩形の範囲に入ってるか？
	bool IsCellInDragSelection(int col, int row) {
		auto l = min(dragStartX, dragEndX);
		auto r = max(dragStartX, dragEndX);
		auto t = min(dragStartY, dragEndY);
		auto b = max(dragStartY, dragEndY);
		return (l <= col && r >= col && t <= row && b >= row);
	}

	//modelIDがこのコンテクストに合致しているかの判断は呼び出しもとに任せる
	bool Undo() {
		if (!undoBuffer->Undoable())
			return false;
		
		int n = undoBuffer->buffer[undoBuffer->index].pack;
		
		for (int i = 0; i < n; i++) {
			solver->Undo(undoBuffer->buffer[undoBuffer->index].undo);
			undoBuffer->index++;
		}
		return true;
	}

	bool Redo() {
		if (!undoBuffer->Redoable())
			return false;

		int n = undoBuffer->buffer[undoBuffer->index-1].pack;
		
		for (int i = 0; i < n; i++) {
			solver->Undo(undoBuffer->buffer[undoBuffer->index - 1].redo);
			undoBuffer->index--;
		}
		return true;
	}

	//直前にアンドゥバッファに積んだredoの内容を実行する(パック数によらず1回だけ)
	void Do() {
		solver->Undo(undoBuffer->buffer[0].redo);
	}

	//subsetをキーフレームに登録する作業を一括で行う
	// desc : 操作履歴窓に表示される操作内容
	// frame : 操作がキーフレームのペーストやロードだった場合、ペースト先のフレーム番号
	// select : 登録したキーだけを選択状態にする。falseの時は選択状態については何も変えない
	// update : キーフレーム表示ウィンドウを更新する
	// ※注意 : updateは当該モデル・カメラが表示されていない時に実行すると、kcに格納されているkeydisp配列がnKeysに合ってなくてクラッシュする
	void RegisterSubset(PMX::KeySubset& subset, const std::wstring& desc, bool select = true, bool update = true, int frame = 0, int pack = 1)
	{
		if (subset.Empty())
			return;
		PMX::KeyUndo undo, redo;
		if (select)
			subset.SelectKeys();
		solver->Revset(subset, frame, undo, redo);
		if (select)
			SelectAllKeys(false);
		undoBuffer->Push(id, undo, redo, name, desc, pack);
		Do();
		status = status | KeyFrameStatus::modify | KeyFrameStatus::check;
		if (update)
			UpdateKeyDisp(status);
	}

	//モデル用、選択ボーンの状態をキーフレームに登録する
	void BoneKeysRegsiter() {
		if (pmx == nullptr)
			return;

		auto psolver = dynamic_cast<PMX::PoseSolver*>(solver);
		PMX::KeySubset sub = {};
		std::wstring putname = L"";
		int nPut = 0;
		for (auto&& node : nodes) {
			for (auto&& leaf : node.leaves) {
				if (leaf.selected) {
					if (leaf.source.index() == 0) {
						//ボーンの場合
						auto key = pose.boneKeys[leaf.index];
						//poseにはAnimFrameにおける状態が入っているのでiFrameもAnimFrameになっている
						//AnimationのスライダーでAnimFrame!=Frameの状態になっている可能性があるので登録フレームをFrameに変更する
						key.iFrame = Frame;	
						sub.motions.push_back({ key, pmx->bones[leaf.index].name });
						if (putname == L"") {
							putname = pmx->bones[leaf.index].name;
						}
						psolver->solvedPose.boneKeys[leaf.index] = key;
						pose.boneKeys[leaf.index].selected = false;
					}
					/* モーフや表示の場合、1つずつしか登録される事が無いのでこのメソッドでは扱わない事にする
						else if (leaf.source.index() == 1) {
						//モーフの場合
						sub.morphs.push_back({ pose.morphKeys[leaf.index], pmx->morphs[leaf.index].name });
						if (putname == L"") {
							putname = pmx->morphs[leaf.index].name;
						}
						psolver->solvedPose.morphKeys[leaf.index] = pose.morphKeys[leaf.index];
						pose.morphKeys[leaf.index].selected = false;
					} else if (leaf.source.index() == 2) {
						//表示・IK・外親
						sub.extras.push_back(pose.extraKey);
						if (putname == L"") {
							putname = L"Extras";
						}
						psolver->solvedPose.extraKey = pose.extraKey;
						pose.extraKey.selected = false;
					}
					*/
					nPut++;
				}
			}
		}

		if (nPut >= 2) {
			putname += std::format(L" +{}key(s)",nPut-1);
		}

		RegisterSubset(sub, std::format(L"put {}", putname));

	}

	//未登録選
	void SelectUpdatedBoneNode()
	{
		std::vector<int>selectedBones;

		for (int i = 0; auto && key : pose.boneKeys) {
			if (key.selected)
				selectedBones.push_back(i);
			i++;
		}

		SelectAll(false);
		for (auto&& node : nodes) {
			for (auto&& leaf : node.leaves) {
				if (leaf.source.index() == 0) {
					for (auto&& idx : selectedBones) {
						if (idx == leaf.index) {
							leaf.selected = true;
						}
					}
				}
			}
		}
	}

	//骨選択, -1で全選択
	void SelectBoneNode(int idx = -1)
	{
		for (auto& node : nodes) 
			for (auto& leaf : node.leaves) 
				if (leaf.source.index() == 0)
					if (leaf.index == idx || idx == -1)
						leaf.selected = true;
	}

	//selectedに選択されているpmx内のボーン番号を返す
	void BonesSelectingStat(std::vector<int>& sel) {
		for (auto && node : nodes) {
			for (auto&& leaf : node.leaves) {
				if (leaf.source.index() == 0 && leaf.selected) {
					sel.push_back(leaf.index);
				}
			}
		}
	}

	//表情選択
	void SelectMorphNode(int idx)
	{
		for (auto& node : nodes)
			for (auto& leaf : node.leaves)
				if (leaf.source.index() == 1 && leaf.index == idx)
					leaf.selected = true;
	}

	//物理ボーンの選択状態
	// 返り値 : 物理ボーンが1つ以上選択されている, trueの場合、以下の情報が有効になる
	// anyON : 1つ以上の選択された物理ボーンがキーフレーム上、有効化されている
	// allON : 全ての選択された物理ボーンが有効化されている
	bool PhysicsBoneSelected(bool& anyON, bool& allON)
	{
		anyON = false;
		allON = false;
		
		if (!pmx)
			return false;

		bool found = false;
		allON = true;
		auto psolver = dynamic_cast<PMX::PoseSolver*>(solver);

		std::vector<int>selected;
		BonesSelectingStat(selected);
		if (!pose.boneKeys.empty()) {
			for (int i : selected) {
				if (pmx->bones[i].IsPhysics) {
					bool phys = pose.boneKeys[i].physics;
					found = true;
					anyON |= phys;
					allON &= phys;
				}
				i++;
			}
		}

		allON &= found;
		return found;
	}
};

//キーフレームマップmの中に指定したフレーム番号の次以降にあるキーのフレーム番号を返す
//無い場合は指定したフレーム番号をそのまま返す
int NextKeyFrame(const KeysVariant m, int frame)
{
	int ret = frame;

	std::visit(
		[&](auto& o) {
			if (!o->empty()) {
				auto it = o->upper_bound(frame);
				if (it != o->end())
					ret = it->second.iFrame;
			}
		}, m
	);
	return ret;
}

//キーフレームマップmの中に指定したフレーム番号の前にあるキーのフレーム番号を返す
//無い場合は指定したフレーム番号をそのまま返す
int PrevKeyFrame(const KeysVariant& m, int frame)
{
	int ret = frame;
	std::visit([&](auto& o) {
		if (!o->empty()) {
			auto it = o->lower_bound(frame);	//まず、frame番かそれ以降で始めのキーへの参照を得る
			if (it == o->end())
				ret = o->rbegin()->second.iFrame;	//frame番以降のキーが見つからない場合、最後に打ってあるフレーム番号を選択
			else if (it != o->begin()) {
				it--;
				ret = it->second.iFrame;
			}	//frameが最初のキーのフレームかそれより前なら、frameをそのまま返す
		}
		}, m
	);
	return ret;
}



//選択状態にあるボーン・表情モーフのうち、隣のキーフレームをさがす
int KeyFrameFindNext(KeyFrameWindowContext& kc, bool forward)
{
	bool found = false;	//選択状態にあるボーン・表情が一つでもあるならtrue
	int f = Frame;	//条件に合致するフレーム番号
	int candy;			//「隣のフレーム」候補
	if (forward) {
		for (auto&& node : kc.nodes) {
			for (auto&& leaf : node.leaves) {
				if (leaf.selected) {
					candy = NextKeyFrame(leaf.source, Frame);
					//挙がってきた候補から元のフレームより動く場所にあって、かつ最も近いフレームを絞る
					if (candy != Frame) {
						if (!found) {
							found = true;
							f = candy;
						} else
							f = min(f, candy);
					}
				}
			}
		}
	} else {
		f = -1;
		for (auto&& node : kc.nodes) {
			for (auto&& leaf : node.leaves) {
				if (leaf.selected) {
					//0フレームには必ずキーが打ってあると仮定する
					found = true;
					f = max(f, PrevKeyFrame(leaf.source, Frame));
				}
			}
		}
	}
	return found ? f : Frame;
}

//現在のセルにキーを描画
void DrawKey(KeyDisp state)
{
	if (state == KeyDisp::none)
		return;

	auto dl = ImGui::GetWindowDrawList();
	bool selected = (state & KeyDisp::select) != KeyDisp::none;

	ImVec4 k = ImLerp(COLSelectedKey1, COLSelectedKey2, sinf((timeGetTime() & 2047) / 2047.0f * 2 * PI) * 0.5 + 0.5);
	ImColor selcol = ImColor(k.x, k.y, k.z);
	ImU32 keycolor = selected ? ImU32(selcol) : ImU32(ImColor(COLKey));
	float cellHeight = ImGui::GetTextLineHeight();
	ImVec2 center = ImGui::GetCursorScreenPos() + ImVec2(KFCellWidth, cellHeight) / 2;
	if ((state & KeyDisp::cross) == KeyDisp::none) {
		dl->AddCircleFilled(center, KFCellWidth * 0.5, keycolor, 4);
	} else {
		float s = KFCellWidth * 0.5;
		dl->AddLine(center + ImVec2(-s, -s), center + ImVec2(+s, +s), keycolor, 2);
		dl->AddLine(center + ImVec2(+s, -s), center + ImVec2(-s, +s), keycolor, 2);
	}
}

//キーフレームの置いてあるorないセルの描画と、セルをクリックした時の挙動
void PutKeyFrameCell(KeyFrameWindowContext& kc, int id, KeyDisp state, bool selectedRow, int column)
{
	auto dl = ImGui::GetWindowDrawList();
	auto& io = ImGui::GetIO();
	int iFrameLeft = max(0, Frame - nKeys / 2);

	//ドラッグ情報更新
	if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) {
		ImVec2 tl = ImGui::GetCursorScreenPos();
		ImVec2 br = tl;
		br.x += ImGui::GetColumnWidth();
		br.y += ImGui::GetTextLineHeightWithSpacing();
		ImVec2 mp = ImGui::GetMousePos();
		if (mp.x >= tl.x && mp.x <= br.x && mp.y >= tl.y && mp.y <= br.y) {
			if (!kc.dragging) {
				kc.dragStartX = ImGui::TableGetColumnIndex();
				kc.dragStartY = ImGui::TableGetRowIndex();
				kc.dragging = true;
			}
			kc.dragEndX = ImGui::TableGetColumnIndex();
			kc.dragEndY = ImGui::TableGetRowIndex();
		}
	} else if (kc.dragging) {
		kc.dragging = false;
		kc.dragFinishing = true;
	}

	if (state != KeyDisp::none) {
		ImGui::PushID(id);
		bool selected = (state & KeyDisp::select) != KeyDisp::none;
		DrawKey(state);
		ImGui::Text(" "); //見えないボタン代わり
		if (ImGui::IsItemClicked()) {
			if (!io.KeyShift)
				kc.SelectAllKeys(false);
			int iNode, iLeaf, iSelectable;
			if (kc.DecodeSelectableID(id, iNode, iLeaf, iSelectable)) {
				//閉じたノードにあるキーをクリックした場合、リーフを探索してマッチしたリーフ上のキーの選択状態を変更する
				//微妙にMMDと違う挙動になる
				//まず、クリックされたセレクタブルのあるフレーム番号を得る
				auto& node = kc.nodes[iNode];
				int found = 0;
				int frame = -1;
				for (int f = 0; f < nKeys; f++) {
					if (node.keydisp[f] != KeyDisp::none) {
						if (found == iSelectable) {
							frame = f + iFrameLeft;
							break;
						}
						found++;
					}
				}
				//フレーム番号に一致するリーフに所属するキーの選択状態を!selectedに合わせる
				for (auto&& leaf : node.leaves) {
					std::visit([&](auto& o) {if (o->contains(frame)) o->at(frame).selected = !selected; }, leaf.source);
				}
			} else {
				auto& leaf = kc.nodes[iNode].leaves[iLeaf];

				int found = 0;
				int frame = -1;
				for (int f = 0; f < nKeys; f++) {
					if (leaf.keydisp[f] != KeyDisp::none) {
						if (found == iSelectable) {
							frame = f + iFrameLeft;
							break;
						}
						found++;
					}
				}

				//リーフに置いてあるキーを直接クリックした場合
				std::visit([&](auto& o) {o->at(frame).selected = !selected; }, leaf.source);
			}
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		ImGui::PopID();
	}

	bool colchanged = false;
	ImVec4 color;

	auto blendLambda = [&](ImVec4 col) { if (colchanged) { color = BlendColor(color, col); } else { color = col; colchanged = true; } };

	if ((column + iFrameLeft) % 5 == 0)
		blendLambda(COLBg5);//5の倍数フレーム

	if (selectedRow)
		blendLambda(COLSelectedBone);	//選択されたボーン

	if (column + iFrameLeft == Frame)
		blendLambda(COLFocusedFrame);	//フォーカスのあるフレーム

	if (kc.dragging)
		if (kc.IsCellInDragSelection(ImGui::TableGetColumnIndex(), ImGui::TableGetRowIndex()))
			blendLambda(COLDragging);	//ドラッグ通過中

	if (colchanged)
		ImGui::TableSetBgColor(ImGuiTableBgTarget_CellBg, ImU32(ImColor(color)));

	ImGui::TableNextColumn();
}


struct KeyFrameWindowResult {
	bool changed = false;	//キーフレームが編集された
	bool undo = false;		//Ctrl+Zが押された
	bool redo = false;		//Ctrl+Yが押された
	bool heavy = false;		//フレームの移動など重い動作があったので今回はレンダリングサボりたい旨
	ImVec2 position;		//ウィンドウの表示位置
};

//ImGuiでMMDのキーフレームの表示をする, キーフレームが編集されるとtrueが返る
KeyFrameWindowResult KeyFrameWindow(KeyFrameWindowContext& kc, bool disabled, PMX::CameraSolver* camSolver = nullptr)
{
	KeyFrameWindowResult ret = {};
	ImGuiIO& io = ImGui::GetIO();
	ImGui::Begin("Keyframes");
	
	ImGui::BeginDisabled(disabled);
	auto dl = ImGui::GetWindowDrawList();

	//カーソルキーとかボタンでフレームの移動
	int prevFrame = Frame;
	if (KBPrevFrame.Is(io,disabled))
			Frame--;
	if (KBPrevPutFrame.Is(io, disabled))
		Frame = KeyFrameFindNext(kc, false);
	if (KBNextFrame.Is(io, disabled))
		Frame++;
	if (KBNextPutFrame.Is(io, disabled))
		Frame = KeyFrameFindNext(kc, true);

	if (ImGui::Button("|<"))
		Frame = 0;
	TooltipDayo("move to 0 frame");

	ImGui::SameLine();
	if (ImGui::Button(".<"))
		Frame = KeyFrameFindNext(kc, false);
	TooltipDayo(KBPrevPutFrame.desc.c_str());

	ImGui::SameLine();
	ImGui::SetNextItemWidth(100);
	ImGui::InputInt("frame", &Frame, 1, 5);
	TooltipDayo("Specify the frame to edit. If you change the frame, changes to unregistered bones, morphs, etc. will be discarded.");

	ImGui::SameLine();
	if (ImGui::Button(">."))
		Frame = KeyFrameFindNext(kc, true);
	TooltipDayo(KBNextPutFrame.desc.c_str());

	ImGui::SameLine();
	if (ImGui::Button(">|"))
		Frame = kc.solver->LastFrame();
	TooltipDayo("move to last frame");

	Frame = max(0, Frame);
	int iFrameLeft = max(0, Frame - nKeys / 2);	//左端のフレーム番号

	//表示範囲が変わった
	if (prevFrame != Frame) {
		ret.changed = true;
		ret.heavy = true;
		kc.UpdateKeyDisp(KeyFrameStatus::frame);
	}

	//キー選択ボタンず
	ImGui::SameLine();
	if (ImGui::Button("all")) {
		//全選択
		kc.SelectAllKeys(true);
		kc.UpdateKeyDisp(KeyFrameStatus::check);
	}
	TooltipDayo("select all keys");

	if (kc.pmx != nullptr) {
		//モデル操作時専用のボタン
		auto psolver = dynamic_cast<PMX::PoseSolver*>(kc.solver);
		ImGui::SameLine();
		if (ImGui::Button("bone")) {
			//ボーンキー選択
			for (auto&& m : psolver->motionKeys)
				for (auto&& key : m)
					key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all bone keys");

		ImGui::SameLine();
		if (ImGui::Button("morph")) {
			//モーフキー選択
			for (auto&& m : psolver->morphKeys)
				for (auto&& key : m)
					key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all morph keys");

		ImGui::SameLine();
		if (ImGui::Button("etc")) {
			//表示・IK・外親
			for (auto&& key : psolver->extraKeys)
				key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all etc keys");

		ImGui::SameLine();
		if (ImGui::Button("physB")) {
			//物理ボーン選択
			kc.SelectAll(false);
			for (int i = 0;  auto && b : kc.pmx->bones) {
				if (b.IsPhysics)
					kc.SelectBoneNode(i);
				i++;
			}
		}
		TooltipDayo("select all physics bones");

		//反転ペースト
		ImGui::SameLine();
		ImGui::BeginDisabled(kc.copyBuffer->subset.motions.empty());
		if (ImGui::Button((char*)u8"⇔paste")) {
			PMX::KeyUndo undo, redo;
			PMX::KeySubset sub = kc.copyBuffer->subset;
			psolver->LRset(sub);
			kc.solver->Revset(sub, Frame, undo, redo);
			kc.undoBuffer->Push(kc.id, undo, redo, kc.name, std::format(L"paste {}F", Frame));
			kc.SelectAllKeys(false);
			kc.Do();
			kc.Reset();
			MessageBeep(MB_ICONINFORMATION);
			ret.changed = true;
		}
		TooltipDayo("paste with left and right reversed");
		ImGui::EndDisabled();

	} else {
		//カメラ・照明モード専用ボタンず
		auto csolver = dynamic_cast<PMX::CameraSolver*>(kc.solver);

		ImGui::SameLine();
		if (ImGui::Button("cam")) {
			for (auto&& key : csolver->cameraKeys)
				key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all camera keys");

		ImGui::SameLine();
		if (ImGui::Button("light")) {
			for (auto&& key : csolver->lightKeys)
				key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all light keys");

		ImGui::SameLine();
		if (ImGui::Button("ss")) {
			for (auto&& key : csolver->selfShadowKeys)
				key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all selfshadow keys");

		ImGui::SameLine();
		if (ImGui::Button("grav")) {
			for (auto&& key : csolver->gravityKeys)
				key.second.selected = true;
			kc.UpdateKeyDisp(KeyFrameStatus::check);
		}
		TooltipDayo("select all gravity keys");
	}

	//範囲選択
	static int frameRange[2] = { 0,-1 };
	static std::string frameRangeDesc = "select keys for the selected bones within the specified frame range";
	auto fromToLambda = [&](int& from, int& to) {from = frameRange[0]; to = frameRange[1]; from = (from < 0) ? 0 : from; to = (to < 0) ? kc.solver->LastFrame() : to; };
	ImGui::SetNextItemWidth(100);
	if (ImGui::InputInt2("range", frameRange)) {
		int from, to;
		fromToLambda(from, to);
		frameRangeDesc = g_hon.Do("select keys for the selected bones within the frame range") + std::format(" {}F - {}F", from, to);
	}
	TooltipDayo("Specify the selection range.\nThe number on the left is the start frame, and the number on the right is the end frame.\nIf you specify -1 for the end frame, it will be considered the frame where the last key was placed.");
	ImGui::SameLine();
	if (ImGui::Button("select")) {
		int from,to;
		fromToLambda(from, to);
		for (auto&& node : kc.nodes) {
			for (auto&& leaf : node.leaves) {
				if (leaf.selected) {
					std::visit([&](auto& o) { for (auto&& [iFrame, value] : *o) { if (from <= iFrame && iFrame <= to) value.selected = true; } }, leaf.source);
				}
			}
		}
		kc.UpdateKeyDisp(KeyFrameStatus::check);
	}
	TooltipDayo(frameRangeDesc.c_str());

	//縦選択
	ImGui::SameLine();
	if (ImGui::Button("vertical")) {
		std::unordered_set<int>f;
		for (auto&& node : kc.nodes) {
			for (auto&& leaf : node.leaves) {
				std::visit([&](auto& o) {  for (auto& [iFrame, value] : *o) if (value.selected) f.insert(iFrame); }, leaf.source);
			}
		}

		for (auto&& i : f) {
			for (auto&& node : kc.nodes) {
				for (auto&& leaf : node.leaves) {
					std::visit([&](auto& o) {
						if (o->contains(i))
							o->at(i).selected = true;
					}, leaf.source);
				}
			}
		}
		kc.UpdateKeyDisp(KeyFrameStatus::check);
	}
	TooltipDayo("makes a key of another bone in the same frame as the selected key selected");


	//コピペとか

	if (!disabled) {
		PMX::KeyUndo undo, redo;

		//コピー
		if (KBCopy.Is(io)) {
			if (kc.solver->AnySelected()) {
				kc.solver->Subset(kc.copyBuffer->subset, true);
				MessageBeep(MB_ICONINFORMATION);
			}
		}

		//ペースト
		if (KBPaste.Is(io)) {
			if (!kc.copyBuffer->subset.Empty()) {
				kc.RegisterSubset(kc.copyBuffer->subset, std::format(L"paste {}F", Frame), true, true, Frame);
				MessageBeep(MB_ICONINFORMATION);
				ret.changed = true;
			}
		}

		//デリート
		if (KBDelete.Is(io)) {
			if (kc.solver->AnySelected()) {
				kc.solver->Revdelset(true, undo, redo);
				kc.undoBuffer->Push(kc.id, undo, redo, kc.name, std::format(L"delete {}F-{}F", undo.rev.FirstFrame(), undo.rev.LastFrame()));
				kc.Do();
				kc.UpdateKeyDisp(KeyFrameStatus::modify | KeyFrameStatus::check);
				ret.changed = true;
			}
		}

		//カット
		if (KBCut.Is(io)) {
			if (kc.solver->AnySelected()) {
				kc.solver->Subset(kc.copyBuffer->subset, true);
				kc.solver->Revdelset(true, undo, redo);
				kc.undoBuffer->Push(kc.id, undo, redo, kc.name, std::format(L"cut {}F-{}F", undo.rev.FirstFrame(), undo.rev.LastFrame()));
				kc.Do();
				kc.UpdateKeyDisp(KeyFrameStatus::modify | KeyFrameStatus::check);
				MessageBeep(MB_ICONINFORMATION);
				ret.changed = true;
			}
		}

		//アンドゥ…モデルの切り替えをする必要があるので、呼び出しもとで処理する
		if (KBUndo.Is(io)) {
			ret.undo = true;
		}

		//リドゥ
		if (KBRedo.Is(io)) {
			ret.redo = true;
		}

		//ボーン選択/解除
		if (KBSelectAll.Is(io)) {
			bool selectedAny = false;
			//表情も込みでなんらかのリーフが選択されていたら全部解除
			for (auto&& node : kc.nodes) {
				for (auto&& leaf : node.leaves) {
					if (leaf.selected) {
						kc.SelectAll(false);
						selectedAny = true;
						break;
					}
				}
				if (selectedAny)
					break;
			}

			if (!selectedAny) {
				//選択されているリーフが無ければボーンだけ全部選択
				for (auto&& node : kc.nodes) {
					for (auto&& leaf : node.leaves) {
						if (leaf.source.index() == 0)
							leaf.selected = true;
					}
				}
			}
		}

		//空フレーム挿入
		bool insertBoneCam = KBInsertBoneCam.Is(io);
		bool insertMorphLight = KBInsertMorphLight.Is(io);
		if (insertBoneCam || insertMorphLight) {
			kc.SelectAllKeys(false);
			//Frame以降のキーを選択
			if (kc.solver->type == PMX::SolverType::camera) {
				auto csolver = dynamic_cast<PMX::CameraSolver*>(kc.solver);
				if (insertBoneCam) {
					for (auto&& [f, val] : csolver->cameraKeys)
						if (f >= Frame)
							val.selected = true;
				} else {
					for (auto&& [f, val] : csolver->lightKeys)
						if (f >= Frame)
							val.selected = true;
				}
			} else {
				auto psolver = dynamic_cast<PMX::PoseSolver*>(kc.solver);
				if (insertBoneCam) {
					for (auto&& mo : psolver->motionKeys)
						for (auto&& [f, val] : mo)
							if (f >= Frame)
								val.selected = true;
				} else {
					for (auto&& mo : psolver->morphKeys)
						for (auto&& [f, val] : mo)
							if (f >= Frame)
								val.selected = true;
				}
			}
			//選択キーのコピーをsubに作る
			PMX::KeySubset sub;
			kc.solver->Subset(sub, false);
			if (!sub.Empty()) {
				std::wstring desc = std::format(L"insert1 {}F", Frame);
				//選択キーをカット
				kc.solver->Revdelset(true, undo, redo);
				kc.undoBuffer->Push(kc.id, undo, redo, kc.name, desc, 2);
				kc.Do();
				//選択キーを1フレーム後ろへペースト
				kc.RegisterSubset(sub, desc, true, true, 1, 2);
				//キーを全部非選択にする
				kc.SelectAllKeys(false);
				kc.UpdateKeyDisp(KeyFrameStatus::modify | KeyFrameStatus::check);
				MessageBeep(MB_ICONINFORMATION);
				ret.changed = true;
			}
		}

		//列フレーム削除
		bool deleteBoneCame = KBDeleteBoneCam.Is(io);
		bool deleteMorphLight = KBDeleteMorphLight.Is(io);
		if (deleteBoneCame || deleteMorphLight) {
			kc.SelectAllKeys(false);
			//Frame以降のキーを選択(ただし、Frameが0だった場合は1から)
			if (kc.solver->type == PMX::SolverType::camera) {
				auto csolver = dynamic_cast<PMX::CameraSolver*>(kc.solver);
				if (deleteBoneCame) {
					for (auto&& [f, val] : csolver->cameraKeys)
						if (f >= Frame && f > 0)
							val.selected = true;
				} else {
					for (auto&& [f, val] : csolver->lightKeys)
						if (f >= Frame && f > 0)
							val.selected = true;
				}
			} else {
				auto psolver = dynamic_cast<PMX::PoseSolver*>(kc.solver);
				if (deleteBoneCame) {
					for (auto&& mo : psolver->motionKeys)
						for (auto&& [f, val] : mo)
							if (f >= Frame && f > 0)
								val.selected = true;
				} else {
					for (auto&& mo : psolver->morphKeys)
						for (auto&& [f, val] : mo)
							if (f >= Frame && f > 0)
								val.selected = true;
				}
			}
			//選択キーのコピーをsubに作る
			PMX::KeySubset sub;
			kc.solver->Subset(sub, false);
			if (!sub.Empty()) {
				std::wstring desc = std::format(L"delete1 {}F", Frame);
				//選択キーをカット
				kc.solver->Revdelset(true, undo, redo);
				kc.undoBuffer->Push(kc.id, undo, redo, kc.name, desc ,2);
				kc.Do();
				//選択キーを1フレーム前へペースト
				kc.RegisterSubset(sub, desc, true, false, -1, 2);
				//キーを全部非選択にする
				kc.SelectAllKeys(false);
				kc.UpdateKeyDisp(KeyFrameStatus::modify | KeyFrameStatus::check);
				MessageBeep(MB_ICONINFORMATION);
				ret.changed = true;
			}
		}

		//選択されたキーAの1つ前に打ってあるキーBを、Aの直前にキーが無ければコピー
		if (KBCopyPrev.Is(io)) {
			PMX::KeySubset paste;
			
			//各キー列で最初に選択されているキーAを探し、その直前に登録されているキーBの内容をコピーしてAの1フレーム前にあるキーとする
			auto backLambda = [&](const auto& m, auto& d) {
				//キーAを探す
				auto it = std::find_if(m.begin(), m.end(), [](auto item) { return item.second.selected;} );
				if (it != m.end() && it != m.begin()) {
					auto f = it->first - 1;	//ペースト先のフレーム番号
					if (f >= 0) {
						it--;
						auto p = *it;	//Aの前に登録されているキーBをコピー
						if (p.first != f) {	//上書になってないかチェック
							p.second.iFrame = f;
							d.push_back(p.second);
						}
					}
				}
			};

			auto backLambda2 = [&](const auto& m, auto& d, const auto& n) {
				//キーAを探す
				auto it = std::find_if(m.begin(), m.end(), [](auto item) { return item.second.selected; });
				if (it != m.end() && it != m.begin()) {
					auto f = it->first - 1;
					if (f >= 0) {
						it--;
						auto p = *it;	//Aの前に登録されているキーBをコピー
						if (p.first != f) {	//上書になってないかチェック
							p.second.iFrame = f;
							d.push_back({ p.second, n });
						}
					}
				}
			};

			if (kc.solver->type == PMX::SolverType::camera) {
				auto csolver = dynamic_cast<PMX::CameraSolver*>(kc.solver);
				backLambda(csolver->cameraKeys, paste.cameras);
				backLambda(csolver->lightKeys, paste.lights);
				backLambda(csolver->selfShadowKeys, paste.selfShadows);
				backLambda(csolver->gravityKeys, paste.gravities);
			} else {
				auto psolver = dynamic_cast<PMX::PoseSolver*>(kc.solver);
				for (int i = 0; auto && m : psolver->motionKeys) {
					backLambda2(m, paste.motions, psolver->pmx->bones[i].name);
					i++;
				}
				for (int i = 0; auto && m : psolver->morphKeys) {
					backLambda2(m, paste.morphs, psolver->pmx->morphs[i].name);
					i++;
				}
				backLambda(psolver->extraKeys, paste.extras);
			}
			if (!paste.Empty()) {
				kc.RegisterSubset(paste, std::format(L"paste {}F", Frame), false, true, 0);
				MessageBeep(MB_ICONINFORMATION);
				ret.changed = true;
			}
		}

	}

	ImGui::Text(" ");	//空行、この列にWaveプロットが入る
	ImGui::Text(" ");	//空行、この列にWaveプロットが入る
	ImVec2 plotPos = ImGui::GetCursorScreenPos() - ImVec2(0, ImGui::GetTextLineHeight()) * 2;

	ImGui::Text(" ");	//空行、この列にフレーム番号が入る
	ImVec2 labelPos = ImGui::GetCursorScreenPos() - ImVec2(0,ImGui::GetTextLineHeight());
	std::vector<ImVec2> frameTextP(nKeys, labelPos);	//フレーム番号書き込み位置保存用

	//表示
	auto tableFlags = ImGuiTableFlags_Resizable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_ScrollX | ImGuiTableFlags_RowBg | ImGuiTableFlags_NoPadInnerX;// | ImGuiTableFlags_NoBordersInBody;
	ImGui::PushStyleColor(ImGuiCol_TableBorderStrong, IM_COL32(0, 0, 0, 0));
	ImGui::PushStyleColor(ImGuiCol_TableBorderLight, IM_COL32(0, 0, 0, 0));
	if (ImGui::BeginTable("keyframes", 1 + nKeys, tableFlags)) {
		bool addcam = g_cfg->alwaysShowCamKeys && kc.pmx;	//モデル編集モードだけどカメラを併記するフラグ

		//ヘッダ
		ImGui::TableSetupScrollFreeze(1, 1 + addcam);
		ImGui::TableSetupColumn("bone", ImGuiTableColumnFlags_NoHide);
		for (int i = 0; i < nKeys; i++) {
			char buf[16];
			_itoa_s(i + iFrameLeft, buf, 15, 10);
			ImGui::TableSetupColumn(buf, ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoResize, KFCellWidth);
		}

		ImGui::TableNextRow();
		ImGui::TableNextColumn();

		//カメラキーの表示
		if (addcam) {
			ImGui::TextUnformatted(g_hon.C("camera"));
			for (int i = 0; i < nKeys; i++) {
				ImGui::TableNextColumn();
				int f = i + iFrameLeft;
				DrawKey(camSolver->cameraKeys.contains(f) ? KeyDisp::has : KeyDisp::none);
			}

			ImGui::TableNextRow();
			ImGui::TableNextColumn();
		}
		ret.position = ImGui::GetCursorScreenPos();

		//prevClickedNode/Leaf～iNode,iLeafまでの選択状態にする
		auto rangeSelectLambda = [&](int iNode, int iLeaf) {
			if (kc.prevClickedNode < 0 || kc.prevClickedNode >= kc.nodes.size())
				return;
			if (kc.prevClickedLeaf < 0 || kc.prevClickedLeaf >= kc.nodes[kc.prevClickedNode].leaves.size())
				return;
			int from = min(kc.prevClickedNode, iNode);
			int to = max(kc.prevClickedNode, iNode);
			int fromLeaf, toLeaf;
			if (from != to) {
				for (int i = from; i <= to; i++) {
					if (i == from) {
						//先頭ノード
						fromLeaf = (kc.prevClickedNode < iNode) ? kc.prevClickedLeaf : iLeaf;
						toLeaf = kc.nodes[i].leaves.size()-1;
					} else if (i == to) {
						//末尾ノード
						fromLeaf = 0;
						toLeaf = (kc.prevClickedNode > iNode) ? kc.prevClickedLeaf : iLeaf; 
					} else {
						//間にあるノード
						fromLeaf = 0;
						toLeaf = kc.nodes[i].leaves.size() - 1;
					}
					for (int j = fromLeaf; j <= toLeaf; j++)
						kc.nodes[i].leaves[j].selected = true;
				}
			} else {
				//先頭ノードと末尾ノードが同じ場合、同じノード内で完結
				fromLeaf = min(kc.prevClickedLeaf, iLeaf);
				toLeaf = max(kc.prevClickedLeaf, iLeaf);
				for (int i = fromLeaf; i <= toLeaf; i++) {
					kc.nodes[from].leaves[i].selected = true;
				}
			}
			kc.prevClickedNode = iNode;
			kc.prevClickedLeaf = iLeaf;
		};

		// リーフ : 個々のボーン、表情モーフの事
		// ノード ：リーフをまとめる親要素の事
		// 
		//ボーン・表情リストの選択仕様…MMD準拠
		//クリック→クリックされたアイテムを選択し、それ以外のアイテムを非選択にする
		//Ctrl+クリック → クリックされたアイテムの選択状態を反転する。それ以外のアイテムについては何もしない
		//Shift+クリック → 最後にクリックされたアイテム～現在クリックされたアイテムの選択状態を反転する
		//閉じたノードの場合はサブアイテムを全て選択・非選択する、サブアイテムが全て「選択」状態だった場合のみ「選択」状態として表示される
		//開いたノードの場合は非選択として表示される

		for (int i = 0; auto && node : kc.nodes) {
			bool nodeOpened = true;
			//directフラグが立っている場合、ノードはツリーとして表示せず、リーフを直接テーブルの行とする
			if (!node.direct) {
				auto baseflags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanFullWidth;
				auto nodeflags = node.Selected() ? (baseflags | ImGuiTreeNodeFlags_Selected) : baseflags;
				nodeOpened = ImGui::TreeNodeEx((void*)(intptr_t)i, nodeflags, node.name.c_str());
				//ノードがクリックされた
				if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
					if (io.KeyCtrl) {
						node.Select(!node.Selected());	//選択されたノードの子を全部選択/解除する
						kc.prevClickedNode = i;
						kc.prevClickedLeaf = 0;
					} else if (io.KeyShift) {
						rangeSelectLambda(i, kc.prevClickedNode < i ? kc.nodes[i].leaves.size()-1 : 0);
					} else {
						kc.SelectAll(false);	//クリックされたノード以外非選択にする
						node.Select(true);
						kc.prevClickedNode = i;
						kc.prevClickedLeaf = 0;
					}
				}
			}
			//ノードが開いている場合リーフの表示
			if (nodeOpened) {
				if (!node.direct) {
					ImGui::TableNextRow();
					ImGui::TableNextColumn();
				}
				for (int j = 0; auto && leaf : node.leaves) {
					leaf.dispRow = ImGui::TableGetRowIndex();
					//各リーフがクリックされた
					if (ImGui::Selectable(leaf.name.c_str(), &leaf.selected)) {
						if (io.KeyShift) {
							rangeSelectLambda(i, j);
							leaf.selected = true;
						} else if (!io.KeyCtrl) {
							kc.SelectAll(false);
							leaf.selected = true;
						}
						kc.prevClickedNode = i;
						kc.prevClickedLeaf = j;
					}
					ImGui::TableNextColumn();
					int iKey = 0;
					for (int x = 0; x < nKeys; x++) {
						frameTextP[x].x = ImGui::GetCursorScreenPos().x;
						PutKeyFrameCell(kc, kc.SelectableID(i, j, iKey), leaf.keydisp[x], leaf.selected, x);
						if (leaf.keydisp[x] != KeyDisp::none)
							iKey++;
					}
					j++;
				}
				if (!node.direct)
					ImGui::TreePop();
			} else {
				//ノードが閉じている場合、リーフのキーのORを表示
				ImGui::TableNextColumn();
				int iKey = 0;
				for (auto&& leaf : node.leaves)
					leaf.dispRow = ImGui::TableGetRowIndex();
				for (int x = 0; x < nKeys; x++) {
					PutKeyFrameCell(kc, kc.SelectableID(i, kc.maxLeaves, iKey), node.keydisp[x], node.Selected(), x);
					if (node.keydisp[x] != KeyDisp::none)
						iKey++;
				}
			}
			i++;
		}
		ImGui::EndTable();
	}
	ImGui::PopStyleColor(2);

	//ドラッグ選択終了した場合、選択範囲の反映
	if (kc.dragFinishing) {
		if (!io.KeyShift)
			kc.SelectAllKeys(false);

		for (auto&& node : kc.nodes)
			for (auto&& leaf : node.leaves)
				for (int i = 0; i < nKeys; i++)
					if (kc.IsCellInDragSelection(i + 1, leaf.dispRow)) {	//ノード・リーフ名で1列占めているので列は+1する
						int f = iFrameLeft + i;
						std::visit([&](auto& o) { if (o->contains(f)) o->at(f).selected = true; }, leaf.source);
					}

		kc.UpdateKeyDisp(KeyFrameStatus::check);
		kc.dragFinishing = false;
	}

	//罫線
	float keiY = ImGui::GetCursorScreenPos().y;
	ImU32 keiColor = ImGui::ColorConvertFloat4ToU32(ImGui::GetStyle().Colors[ImGuiCol_Border]);
	float hcw = KFCellWidth / 2.0f;	//セルの幅の半分
	for (int i = 0; i < nKeys; i++) {
		auto p = frameTextP[i];
		p.x += hcw;
		dl->AddLine(p, ImVec2(p.x, keiY), keiColor);
	}

	//フレーム番号書き入れ
	ImU32 fontCol = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);
	for (int i = 0; i < nKeys; i++) {
		int f = iFrameLeft + i;
		if (f % 5 == 0) {
			std::string s = std::format("{}", f).c_str();
			auto ts = ImGui::CalcTextSize(s.c_str());
			dl->AddText(nullptr, min(KFCellWidth * 2, 12), frameTextP[i] + ImVec2(hcw-ts.x/2.0f,0), fontCol, s.c_str());
		}
	}

	//波形プロット
	if (!g_wave.PCM.empty()) {
		plotPos.x = frameTextP[0].x;
		ImGui::SetCursorScreenPos(plotPos);
		int cw = KFCellWidth + 1;	//セルの幅+罫線分1
		int size = cw * nKeys;
		int offset = iFrameLeft * cw - hcw + (int)(g_wave.Freq()*2/(30.0f*cw)*kc.wavOffset);
		int h = ImGui::GetTextLineHeight() * 3;
		auto color = ImGui::GetStyle().Colors[ImGuiCol_Button];
		for (int x = 0; x < size; x++) {
			int l = plotPos.x + x;
			int idx = x + offset;
			int t = 0, b = 0;
			if (idx < g_wavePlotMax.size() && idx>=0) {
				t = plotPos.y + h * g_wavePlotMax[idx];
				b = plotPos.y + h * g_wavePlotMin[idx];
			}
			dl->AddLine(ImVec2(l, t), ImVec2(l, b), ImColor(color));
		}
	}

	//ウィンドウサイズの変更に伴ってnKeysを変更する
	int nextKeys = max(1,(ImGui::GetWindowContentRegionMax().x + ImGui::GetWindowPos().x - frameTextP[0].x) / (KFCellWidth+1));
	if (nextKeys != nKeys) {
		nKeys = nextKeys;
		kc.Reset();
		//YRZ::DEB(L"nKeys:{}", nKeys);
	}

	ImGui::EndDisabled();

	ImGui::End();


	return ret;
}

