#include "Editor/CVar.h"

namespace he {

static TArray<CVarBase*>& GetCVarList() {
    static TArray<CVarBase*> s_List;
    return s_List;
}

CVarBase::CVarBase(StringView name, StringView desc)
    : m_Name(name), m_Description(desc) {
    GetCVarList().push_back(this);
}

const TArray<CVarBase*>& CVarBase::GetAll() {
    return GetCVarList();
}

CVarBase* FindCVar(StringView name) {
    for (auto* cvar : GetCVarList()) {
        if (cvar->GetName() == name) return cvar;
    }
    return nullptr;
}

} // namespace he
