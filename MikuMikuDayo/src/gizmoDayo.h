//ボーン編集ウィンドウ
#pragma once

#include "defsDayo.h"

struct GizmoState {
//   vec3 mSnapScale = { 1,1,1 };
    ImGuizmo::OPERATION op = ImGuizmo::TRANSLATE;
    ImGuizmo::MODE mode = ImGuizmo::LOCAL;
    bool snap = false;
    vec3 snapBounds;
};

GizmoState g_gizmoState;

struct BoneWindowResult {
    bool updated = false;       //ギズモが直接入力により更新された
    bool registered = false;    //ボーン登録ボタンが押された
    bool revert = false;        //巻き戻すボタンが押された
    bool init = false;          //初期化ボタンが押された
    bool selectUnregisted = false;  //未登録選ボタン
    bool phys = false;          //物理チェックがクリックされた
};


//360°系の角度を0～359°の範囲にする
float NormalizeAngle(float angle)
{
    float a = fmodf(angle, 360.0f);
    return a < 0 ? a + 360.0f : a;
}

vec3 NormalizeAngle(const vec3& a)
{
    return vec3(NormalizeAngle(a.x), NormalizeAngle(a.y), NormalizeAngle(a.z));
}

//角度を90°-270°線で反転する
float Flip90_270(float a)
{
    float x = NormalizeAngle(a);
    if (x <= 180) {
        return 180 - x;
    } else {
        return 540 - x;
    }
}

//e1とe2のYZ要素が両方反転しただけで同じ姿勢を表しているか調べる
//vecQ::ToEuler().xは360°系に直した場合、0～90、270～360の範囲の値のみ返す
//yz要素を180°反転させることでそうなるように調整する
//↑のような仕様になっていると思われる、ToEuler上で同じ姿勢かどうかだけ判定するコードである。つまり汎用性はない
bool IdenticalOrientation(const vec3& e1, const vec3& e2, float eps = 1e-3)
{
    vec3 a = NormalizeAngle(e1);
    vec3 b = NormalizeAngle(e2);
    vec3 c = NormalizeAngle(e2 + vec3(0, 180, 180));

    bool yzSame =  abs(a.y - b.y) < eps && abs(a.z - b.z) < eps;    //YZは等しい
    if (yzSame) {
        return abs(a.x - b.x) < eps;
    }

    bool yzRev = abs(a.y - c.y) < eps && abs(a.z - c.z) < eps;  //YZは丁度反転している
    if (yzRev) {
        return abs(Flip90_270(a.x) - c.x) < eps;    //円周上の90°-270°の線に対してちょうど対称の位置にある？
    }

    return false;
}


//ボーン設定ウィンドウ、直接入力された場合はtrueが返る
BoneWindowResult BoneWindow(bool editable, bool validKC, vec3& boneP, vecQ& boneQ, KeyFrameWindowContext* kc, const std::vector<int>& selectedBones, bool gizmoed, bool& boneSelection, int iModel)
{
    ImGuiIO& io = ImGui::GetIO();
    BoneWindowResult br = {};
    PMX::PMXBone* pmxb = nullptr; //編集対象のPMXボーン(selectedBonesの数が1の時のみ有効)

    ImGui::Begin("Bone");

    if (!editable)
        ImGui::BeginDisabled();
    else
        pmxb = &kc->pmx->bones[*selectedBones.begin()];

    //回転/移動禁止ボーン
    if (pmxb && !pmxb->IsTranslation())
        g_gizmoState.op = ImGuizmo::ROTATE;
    else if (pmxb && !pmxb->IsRotation())
        g_gizmoState.op = ImGuizmo::TRANSLATE;

    if (pmxb && KBTranslation.Is(io) && pmxb->IsTranslation())
        g_gizmoState.op = ImGuizmo::TRANSLATE;
    if (pmxb && KBRotation.Is(io) && pmxb->IsRotation())
        g_gizmoState.op = ImGuizmo::ROTATE;


    if (pmxb && pmxb->IsTranslation()) {
        if (ImGui::RadioButton("Translate", g_gizmoState.op == ImGuizmo::TRANSLATE))
            g_gizmoState.op = ImGuizmo::TRANSLATE;
        TooltipDayo(KBTranslation.desc.c_str());
    }
        

    if (pmxb && pmxb->IsRotation()) {
        ImGui::SameLine();
        if (ImGui::RadioButton("Rotate", g_gizmoState.op == ImGuizmo::ROTATE))
            g_gizmoState.op = ImGuizmo::ROTATE;
        TooltipDayo(KBRotation.desc.c_str());
    }

    //回転・移動禁止ボーンでも数値入力なら無視して動かせる
    vec3 inputTr = {};
    static vec3 inputRt = {};
    //X軸>90°でプルプル問題
    // ①inputRtの結果が(91,0,0)とかになってると、ToEulerで返ってくる値は(1,180,180)など、X軸が±90°を越えない値で返ってくる
    // ②inputFloatのX要素にはX=91が入ったままでY,Zには180,180が入る
    // ③次のフレームでは(91,180,180)で入力されたと扱われる
    // 上記の繰り返しになるのでプルプルする
    // これを防ぐため入力欄の数値上の姿勢とboneQで指定された姿勢が、オイラー角表示上は違っても同じ姿勢であれば、入力欄の更新はしない
    vec3 eulerNow = boneQ.ToEuler() * 180.0f / PI;
    inputRt = IdenticalOrientation(inputRt, eulerNow) ? inputRt : eulerNow;
    inputTr = boneP;
    br.updated |= ImGui::InputFloat3("position", (float*)&inputTr);
    br.updated |= ImGui::InputFloat3("rotation", (float*)&inputRt);
    if (br.updated) {
        boneQ = vecQ::CreateFromYawPitchRoll(inputRt * PI / 180.0f);
        boneP = inputTr;
    }

    //捩じりボーンによる軸制限…ギズモによって動かされた時だけ反応する
    if (pmxb && pmxb->IsFixAxis() && gizmoed) {
        vecQ q = boneQ;
        vec3 axis = Normalize(xyz(q));
        float c = Dot(axis, pmxb->fixAxis) * q.w;
        float s = sqrtf(1.0f - c * c);
        vecQ nq = Normalize(vecQ(xyzw(s*pmxb->fixAxis, c)));
        boneQ = nq;
    }

    if (g_gizmoState.op != ImGuizmo::SCALE) {
        if (ImGui::RadioButton("Local", g_gizmoState.mode == ImGuizmo::LOCAL))
            g_gizmoState.mode = ImGuizmo::LOCAL;
        TooltipDayo("operate in this bone's own local coordinate system %s", KBLocalWorld.disp);
        ImGui::SameLine();
        if (ImGui::RadioButton("World", g_gizmoState.mode == ImGuizmo::WORLD))
            g_gizmoState.mode = ImGuizmo::WORLD;
        TooltipDayo("operate in global coordinate system %s", KBLocalWorld.disp);
    }

    vec3 snap = { 0,0,0 };
    switch (g_gizmoState.op) {
    case ImGuizmo::TRANSLATE: snap = vec3(g_cfg->snapTranslation); break;
    case ImGuizmo::ROTATE: snap = vec3(g_cfg->snapRotation); break;
    /*
    case ImGuizmo::SCALE:
        snap = &g_gizmoState.mSnapScale;
        ImGui::InputFloat("Scale Snap", reinterpret_cast<float*>(snap));
        break;
        */
    }
    g_gizmoState.snap = io.KeyCtrl;
    g_gizmoState.snapBounds = snap;

    if (!editable)
        ImGui::EndDisabled();

    ImGui::Checkbox("select", &boneSelection);
    TooltipDayo(KBBoneSelect.desc.c_str());
    if (KBBoneSelect.Is(io)) {
        boneSelection = !boneSelection;
    }

    ImGui::SameLine();
    if (br.selectUnregisted = (ImGui::Button("unregisted") || KBUnregisted.Is(io))) {
        kc->SelectUpdatedBoneNode();
    }
    TooltipDayo(KBUnregisted.desc.c_str());

    static bool phys = true;
    if (!selectedBones.empty()) {
        //物理ボーンが含まれる場合は物理チェックボックスを有効に
        if (validKC) {
            bool anyON, allON;
            ImGui::BeginDisabled(!kc->PhysicsBoneSelected(anyON, allON));
            phys = anyON;
            br.phys = ImGui::Checkbox("physics", &phys);
            ImGui::EndDisabled();
        }

        br.registered = ImGui::Button("register") || KBRegister.Is(io);
        TooltipDayo(KBRegister.desc.c_str());

        ImGui::SameLine();
        br.revert = ImGui::Button("revert");

        ImGui::SameLine();
        br.init = ImGui::Button("init");
    }


    ImGui::End();


    if ((br.updated || br.init || br.revert || gizmoed) && validKC) {
        auto solver = dynamic_cast<PMX::PoseSolver*>(kc->solver);
        Vector3 t;
        Quaternion q;

        for (auto&& iBone : selectedBones) {

            auto setKeyLambda = [&](bool select) {
                PMX::VMDMotionLite& key = kc->pose.boneKeys[iBone];
                key.position = t;
                key.rotation = q;
                key.selected = select;	//更新されたフラグを立てる
                };

            if (br.init) {
                //初期化
                t = {};	q = {};
                setKeyLambda(true);
            } else if (br.revert) {
                //solvedPoseまで巻き戻す...physPoseでなくていい?
                t = solver->solvedPose.boneKeys[iBone].position;
                q = solver->solvedPose.boneKeys[iBone].rotation;
                setKeyLambda(false);
            } else if (selectedBones.size() == 1) {
                //指定された値にする(選択ボーンが1個だった時専用)
                q = boneQ;
                t = boneP;
                setKeyLambda(true);
            }
        }
        if (!selectedBones.empty()) {
            g_dayo->PoseUpdate(true, iModel);   //ポーズを表示用の姿勢に反映…TODO : 物理はsolvedPoseを基準にするので物理と矛盾が出る
            g_dayo->cb->iSample = 0;
        }
    }

    //物理チェック
    if (br.phys && validKC) {
        for (auto&& iBone : selectedBones) {
            if (kc->pmx->bones[iBone].IsPhysics) {
                kc->pose.boneKeys[iBone].physics = phys;
            }
        }
        if (!selectedBones.empty()) {
            g_dayo->PoseUpdate();   //ポーズを表示用の姿勢に反映
            g_dayo->cb->iSample = 0;
        }
    }

    if (br.registered && validKC) {
        kc->BoneKeysRegsiter();
    }

    return br;
}

//ギズモ起動！
//imgなんとか…レンダラー窓の四隅
bool Gizmo(KeyFrameWindowContext* kc, int iBone, const Matrix& view, const Matrix& proj, Matrix& matrix, int imgX, int imgY, int imgW, int imgH)
{
    ImGuizmo::SetDrawlist();

    ImGuiIO& io = ImGui::GetIO();
    ImVec2 winTL = ImGui::GetWindowPos();
    ImGuizmo::SetRect(winTL.x + imgX, winTL.y + imgY, imgW, imgH);

    //Matrix m = {};
    //ImGuizmo::DrawGrid(&view._11, &proj._11, &m._11, 100.f);

    //右クリックでキーフレーム操作メニュー
    if (ImGuizmo::IsOver() && io.MouseClicked[1]) {
        ImGui::OpenPopup("gizmo");
    }
    if (ImGui::BeginPopup("gizmo")) {
        auto solver = dynamic_cast<PMX::PoseSolver*>(kc->solver);

        if (ImGui::Selectable("register")) {
            kc->BoneKeysRegsiter();
            ImGui::CloseCurrentPopup();
        }
        
        vec3 t;
        vecQ q;
        auto setKeyLambda = [&](bool select) {
            PMX::VMDMotionLite& key = kc->pose.boneKeys[iBone];
            key.position = t;
            key.rotation = q;
            key.selected = select;	//更新されたフラグを立てる
            g_dayo->PoseUpdate();
            g_dayo->cb->iSample = 0;
        };

        if (ImGui::Selectable("revert")) {
            t = solver->solvedPose.boneKeys[iBone].position;
            q = solver->solvedPose.boneKeys[iBone].rotation;
            setKeyLambda(false);
            ImGui::CloseCurrentPopup();
        };

        if (ImGui::Selectable("init")) {
            t = {}; q = {};
            setKeyLambda(true);
            ImGui::CloseCurrentPopup();
        };
        
        ImGui::EndPopup();
    }

    bool ret = ImGuizmo::Manipulate(&view._11, &proj._11, g_gizmoState.op, g_gizmoState.mode, &matrix._11, NULL, g_gizmoState.snap ? &g_gizmoState.snapBounds.x : NULL);
    return ret;
}