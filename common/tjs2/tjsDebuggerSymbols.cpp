//---------------------------------------------------------------------------
/*
	TJS2 Script Engine
	Copyright (C) 2000 W.Dee <dee@kikyou.info> and contributors

	See details of license at "license.txt"
*/
//---------------------------------------------------------------------------
// Debugger symbol tables.
// コンパイル時 (tjsInterCodeGen) に名前→id マップとローカル/クラス変数の
// レジスタ位置情報を蓄積し、ブレーク時に変数 inspect で参照する。
// プラットフォーム非依存 (元 tjsDebug.cpp 内の Debugger クラスから分離)。
//---------------------------------------------------------------------------
#include "tjsCommHead.h"

#include <map>
#include <list>
#include <string>
#include <vector>
#include <cassert>

#include "tjsDebug.h"
#include "tjsInterCodeGen.h"
#include "tjsUtils.h"

namespace TJS
{

//---------------------------------------------------------------------------
// 名前コレクション: ファイル名/クラス名/関数名/変数名を id に変換
//---------------------------------------------------------------------------
class NameIndexCollection
{
protected:
	std::map<tjs_string, int> NameWithID;
	std::vector<const tjs_string*> Names;

public:
	int GetID(const tjs_string& name)
	{
		std::map<tjs_string, int>::const_iterator i = NameWithID.find(name);
		if (i != NameWithID.end()) return i->second;
		// 見付からないので追加
		int index = (int)Names.size();
		typedef std::pair<std::map<tjs_string, int>::iterator, bool> name_result_t;
		name_result_t ret = NameWithID.insert(std::make_pair(name, index));
		assert(ret.second);
		const tjs_string* name_ref = &((*(ret.first)).first);
		Names.push_back(name_ref);
		return index;
	}
	const tjs_string* GetName(int id) const
	{
		if (id < (int)Names.size()) return Names[id];
		return NULL;
	}
};
static NameIndexCollection NameIndexCollectionData;

static int TJSGetIDFromName(const tjs_char* name)
{
	return NameIndexCollectionData.GetID(tjs_string(name));
}
static const tjs_string* TJSGetNameFromID(int id)
{
	return NameIndexCollectionData.GetName(id);
}

//---------------------------------------------------------------------------
void TJSDebuggerGetScopeKey(ScopeKey& scope, const tjs_char* classname,
		const tjs_char* funcname, const tjs_char* filename, int codeoffset)
{
	int classindex = -1;
	if (classname && classname[0]) classindex = TJSGetIDFromName(classname);
	int funcindex = -1;
	if (funcname && funcname[0]) funcindex = TJSGetIDFromName(funcname);
	int fileindex = -1;
	if (filename && filename[0]) fileindex = TJSGetIDFromName(filename);
	scope.Set(classindex, funcindex, fileindex, codeoffset);
}

//---------------------------------------------------------------------------
struct LocalVariableKey
{
	int VarIndex;
	int RegAddr;
	LocalVariableKey(int var, int reg) : VarIndex(var), RegAddr(reg) {}
	LocalVariableKey() : VarIndex(-1), RegAddr(-1) {}
	LocalVariableKey(const LocalVariableKey& rhs)
	{
		VarIndex = rhs.VarIndex;
		RegAddr = rhs.RegAddr;
	}
	void Set(int var, int reg) { VarIndex = var; RegAddr = reg; }
};

//---------------------------------------------------------------------------
class ClassVariableCollection
{
	typedef LocalVariableKey ClassVariableKey;
	typedef std::map<int, std::list<ClassVariableKey> > variable_map_t;
	typedef variable_map_t::iterator iterator;
	variable_map_t Variables;

public:
	void ClearVar(const tjs_char* classname)
	{
		int classindex = -1;
		if (classname && classname[0]) classindex = TJSGetIDFromName(classname);
		variable_map_t::iterator i = Variables.find(classindex);
		if (i != Variables.end()) i->second.clear();
	}

	void SetVar(const tjs_char* classname, const tjs_char* varname, int regaddr)
	{
		int classindex = -1;
		if (classname && classname[0]) classindex = TJSGetIDFromName(classname);
		int varindex = -1;
		if (varname && varname[0]) varindex = TJSGetIDFromName(varname);
		variable_map_t::iterator i = Variables.find(classindex);
		if (i != Variables.end()) {
			i->second.push_back(ClassVariableKey(varindex, regaddr));
			return;
		}
		typedef std::pair<iterator, bool> result_t;
		result_t ret = Variables.insert(std::make_pair(classindex, std::list<ClassVariableKey>()));
		assert(ret.second);
		ret.first->second.push_back(LocalVariableKey(varindex, regaddr));
	}

	// 変数名と値のリストを得る (name, value, name, value, ... の交互列)
	// variant 版 (Phase 5a): 値を生で返す
	void GetVarsRaw(const tjs_char* classname, tTJSVariant* ra, tTJSVariant* da,
					std::vector<TJSDebuggerVar>& out)
	{
		out.clear();
		if (ra == NULL || da == NULL) return;
		int classindex = -1;
		if (classname && classname[0]) classindex = TJSGetIDFromName(classname);
		iterator i = Variables.find(classindex);
		if (i == Variables.end()) return;
		const std::list<ClassVariableKey>& vars = i->second;
		for (auto j = vars.begin(); j != vars.end(); ++j) {
			const tjs_string* varname = TJSGetNameFromID((*j).VarIndex);
			if (!varname) continue;
			tTJSVariant* ra_code1 = TJS_GET_VM_REG_ADDR(ra, TJS_TO_VM_REG_ADDR(-1));
			if (ra_code1->Type() != tvtObject || !ra_code1->AsObjectNoAddRef()) continue;
			tTJSVariantClosure clo = ra_code1->AsObjectClosureNoAddRef();
			if (!clo.Object) continue;
			tTJSVariant result;
			tjs_error hr = clo.PropGet(0, varname->c_str(), NULL, &result,
				clo.ObjThis ? clo.ObjThis : ra[-1].AsObjectNoAddRef());
			if (TJS_FAILED(hr)) continue;
			TJSDebuggerVar v;
			v.name  = *varname;
			v.value = result;
			out.push_back(std::move(v));
		}
	}

	void GetVars(const tjs_char* classname, tTJSVariant* ra, tTJSVariant* da,
				 std::list<tjs_string>& values)
	{
		values.clear();
		if (ra == NULL || da == NULL) return;

		int classindex = -1;
		if (classname && classname[0]) classindex = TJSGetIDFromName(classname);

		iterator i = Variables.find(classindex);
		if (i != Variables.end()) {
			tjs_string selfclass(TJS_W("this."));
			if (classindex < 0) selfclass = tjs_string(TJS_W("global."));
			const std::list<ClassVariableKey>& vars = i->second;
			for (std::list<ClassVariableKey>::const_iterator j = vars.begin();
				 j != vars.end(); ++j) {
				const tjs_string* varname = TJSGetNameFromID((*j).VarIndex);
				tjs_string memname;
				if (varname) memname = selfclass + *varname;
				else          memname = selfclass + tjs_string(TJS_W("(unknown)"));

				tTJSVariant* ra_code1 = TJS_GET_VM_REG_ADDR(ra, TJS_TO_VM_REG_ADDR(-1));
				tjs_error hr = -1;
				tTJSVariant result;
				if (varname && ra_code1->Type() == tvtObject && ra_code1->AsObjectNoAddRef()) {
					tTJSVariantClosure clo = ra_code1->AsObjectClosureNoAddRef();
					if (clo.Object) {
						hr = clo.PropGet(0, varname->c_str(), NULL, &result,
							clo.ObjThis ? clo.ObjThis : ra[-1].AsObjectNoAddRef());
					} else {
						hr = -1;
					}
				}

				tjs_string value;
				if (TJS_FAILED(hr)) value = tjs_string(TJS_W("error"));
				else                value = tjs_string(TJSVariantToReadableString(result).c_str());

				values.push_back(memname);
				values.push_back(value);
			}
		}
	}
};

//---------------------------------------------------------------------------
class LocalVariableCollection
{
	typedef std::map<ScopeKey, std::list<LocalVariableKey> > variable_map_t;
	typedef variable_map_t::iterator iterator;
	variable_map_t Variables;

public:
	void SetVar(const ScopeKey& scope, const tjs_char* varname, int regaddr)
	{
		int varindex = -1;
		if (varname && varname[0]) varindex = TJSGetIDFromName(varname);

		iterator i = Variables.find(scope);
		if (i != Variables.end()) {
			i->second.push_back(LocalVariableKey(varindex, regaddr));
			return;
		}
		typedef std::pair<iterator, bool> result_t;
		result_t ret = Variables.insert(std::make_pair(scope, std::list<LocalVariableKey>()));
		assert(ret.second);
		ret.first->second.push_back(LocalVariableKey(varindex, regaddr));
	}

	void SetVar(const tjs_char* classname, const tjs_char* funcname,
				const tjs_char* filename, int codeoffset,
				const tjs_char* varname, int regaddr)
	{
		ScopeKey scope;
		TJSDebuggerGetScopeKey(scope, classname, funcname, filename, codeoffset);
		SetVar(scope, varname, regaddr);
	}

	void ClearVar(const ScopeKey& scope)
	{
		iterator i = Variables.find(scope);
		if (i != Variables.end()) i->second.clear();
	}
	void ClearVar(const tjs_char* classname, const tjs_char* funcname,
				  const tjs_char* filename, int codeoffset)
	{
		ScopeKey scope;
		TJSDebuggerGetScopeKey(scope, classname, funcname, filename, codeoffset);
		ClearVar(scope);
	}

	// variant 版 (Phase 5a): 値を生で返す
	void GetVarsRaw(const ScopeKey& scope, tTJSVariant* ra,
					std::vector<TJSDebuggerVar>& out)
	{
		out.clear();
		if (ra == NULL) return;
		iterator i = Variables.find(scope);
		if (i == Variables.end()) return;
		const std::list<LocalVariableKey>& vars = i->second;
		for (auto j = vars.begin(); j != vars.end(); ++j) {
			const tjs_string* varname = TJSGetNameFromID((*j).VarIndex);
			TJSDebuggerVar v;
			v.name  = varname ? *varname : tjs_string(TJS_W("(unknown)"));
			v.value = TJS_GET_VM_REG(ra, (*j).RegAddr);
			out.push_back(std::move(v));
		}
	}

	void GetVars(const ScopeKey& scope, tTJSVariant* ra, std::list<tjs_string>& values)
	{
		values.clear();
		if (ra == NULL) return;
		iterator i = Variables.find(scope);
		if (i != Variables.end()) {
			const std::list<LocalVariableKey>& vars = i->second;
			for (std::list<LocalVariableKey>::const_iterator j = vars.begin();
				 j != vars.end(); ++j) {
				const tjs_string* varname = TJSGetNameFromID((*j).VarIndex);
				tjs_string name;
				if (varname) name = *varname;
				else          name = tjs_string(TJS_W("(unknown)"));
				tjs_string value(TJSVariantToReadableString(TJS_GET_VM_REG(ra, (*j).RegAddr)).c_str());
				values.push_back(name);
				values.push_back(value);
			}
		}
	}
	void GetVars(const tjs_char* classname, const tjs_char* funcname,
				 const tjs_char* filename, int codeoffset,
				 tTJSVariant* ra, std::list<tjs_string>& values)
	{
		ScopeKey scope;
		TJSDebuggerGetScopeKey(scope, classname, funcname, filename, codeoffset);
		GetVars(scope, ra, values);
	}
};

static LocalVariableCollection LocalVariableCollectionData;
static ClassVariableCollection ClassVariableCollectionData;

//---------------------------------------------------------------------------
// public API (tjsDebug.h で宣言)
//---------------------------------------------------------------------------
// codeoffset は関数コール前のコードオフセット
void TJSDebuggerAddLocalVariable(const tjs_char* classname, const tjs_char* funcname,
		const tjs_char* filename, int codeoffset, const tjs_char* varname, int regaddr)
{
	LocalVariableCollectionData.SetVar(classname, funcname, filename, codeoffset, varname, regaddr);
}
void TJSDebuggerGetLocalVariableString(const tjs_char* classname, const tjs_char* funcname,
		const tjs_char* filename, int codeoffset, tTJSVariant* ra, std::list<tjs_string>& values)
{
	LocalVariableCollectionData.GetVars(classname, funcname, filename, codeoffset, ra, values);
}
void TJSDebuggerAddLocalVariable(const ScopeKey& key, const tjs_char* varname, int regaddr)
{
	LocalVariableCollectionData.SetVar(key, varname, regaddr);
}
void TJSDebuggerGetLocalVariableString(const ScopeKey& key, tTJSVariant* ra,
		std::list<tjs_string>& values)
{
	LocalVariableCollectionData.GetVars(key, ra, values);
}
void TJSDebuggerClearLocalVariable(const ScopeKey& key)
{
	LocalVariableCollectionData.ClearVar(key);
}
void TJSDebuggerClearLocalVariable(const tjs_char* classname, const tjs_char* funcname,
		const tjs_char* filename, int codeoffset)
{
	LocalVariableCollectionData.ClearVar(classname, funcname, filename, codeoffset);
}

// クラスメンバ変数の初期化
void TJSDebuggerAddClassVariable(const tjs_char* classname, const tjs_char* varname, int regaddr)
{
	ClassVariableCollectionData.SetVar(classname, varname, regaddr);
}
void TJSDebuggerGetClassVariableString(const tjs_char* classname, tTJSVariant* ra,
		tTJSVariant* da, std::list<tjs_string>& values)
{
	ClassVariableCollectionData.GetVars(classname, ra, da, values);
}
void TJSDebuggerClearLocalVariable(const tjs_char* classname)
{
	ClassVariableCollectionData.ClearVar(classname);
}

//---------------------------------------------------------------------------
// variant ベース API (Phase 5a): 子オブジェクト展開のため値そのものを返す。
//---------------------------------------------------------------------------
void TJSDebuggerGetLocalVariables(const ScopeKey& key, tTJSVariant* ra,
		std::vector<TJSDebuggerVar>& out)
{
	LocalVariableCollectionData.GetVarsRaw(key, ra, out);
}
void TJSDebuggerGetClassVariables(const tjs_char* classname, tTJSVariant* ra,
		tTJSVariant* da, std::vector<TJSDebuggerVar>& out)
{
	ClassVariableCollectionData.GetVarsRaw(classname, ra, da, out);
}

} // namespace TJS
