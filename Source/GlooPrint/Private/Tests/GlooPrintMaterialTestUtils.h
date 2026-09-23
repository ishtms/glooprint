// Copyright 2026 Ishtmeet Singh. All Rights Reserved.

#pragma once
#if WITH_DEV_AUTOMATION_TESTS

#include "GlooPrintGraphAdapter.h"
#include "GlooPrintTestUtils.h"
#include "MaterialEditingLibrary.h"
#include "MaterialGraph/MaterialGraph.h"
#include "MaterialGraph/MaterialGraphNode.h"
#include "MaterialGraph/MaterialGraphNode_Root.h"
#include "MaterialGraph/MaterialGraphSchema.h"
#include "Materials/MaterialExpressionAdd.h"
#include "Materials/MaterialExpressionConstant.h"
#include "Materials/MaterialExpressionComment.h"
#include "Materials/MaterialExpressionCustom.h"
#include "Materials/MaterialExpressionFunctionInput.h"
#include "Materials/MaterialExpressionMaterialFunctionCall.h"
#include "Materials/MaterialExpressionMultiply.h"
#include "Materials/MaterialExpressionFunctionOutput.h"
#include "Materials/MaterialExpressionMakeMaterialAttributes.h"
#include "Materials/MaterialFunction.h"
#include "Materials/MaterialFunctionMaterialLayer.h"
#include "Materials/MaterialFunctionMaterialLayerBlend.h"
#include "UObject/Package.h"

namespace GlooPrint::Tests
{
struct FMaterialFixture
{
    TStrongObjectPtr<UMaterial> Material;
    TStrongObjectPtr<UMaterialFunction> Function;
    TStrongObjectPtr<UMaterialFunction> CalledFunction;
    UMaterialGraph* Graph = nullptr;
    UMaterialExpressionConstant* Constant = nullptr;
    UMaterialExpressionAdd* Add = nullptr;
    UMaterialExpressionFunctionOutput* Output = nullptr;

    explicit FMaterialFixture(int32 Family = 0, int32 Count = 8, UPackage* Package = nullptr, bool bMixed = false)
    {
        if (!Package) { Package = GetTransientPackage(); }
        Material.Reset(NewObject<UMaterial>(Package, NAME_None, RF_Transactional));
        if (Family)
        {
            UClass* Type = Family == 2 ? UMaterialFunctionMaterialLayer::StaticClass() :
                Family == 3 ? UMaterialFunctionMaterialLayerBlend::StaticClass() : UMaterialFunction::StaticClass();
            Function.Reset(NewObject<UMaterialFunction>(Package, Type, NAME_None, RF_Transactional));
        }
        Constant = Expression<UMaterialExpressionConstant>(500, 400); Constant->R = 0.375f;
        if (bMixed)
        {
            CalledFunction.Reset(NewObject<UMaterialFunction>(Package, NAME_None, RF_Transactional));
            auto* Input = CastChecked<UMaterialExpressionFunctionInput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                CalledFunction.Get(), UMaterialExpressionFunctionInput::StaticClass(), 0, 0));
            Input->InputType = FunctionInput_Scalar;
            auto* Result = CastChecked<UMaterialExpressionFunctionOutput>(UMaterialEditingLibrary::CreateMaterialExpressionInFunction(
                CalledFunction.Get(), UMaterialExpressionFunctionOutput::StaticClass(), 400, 0));
            Result->A.Connect(0, Input);
        }
        UMaterialExpression* Previous = Constant;
        for (int32 I = 1; I < Count - (bMixed ? 2 : 1); ++I)
        {
            const int32 X = (I % 5) * 400 - 1000, Y = (I / 5) * 270;
            if (bMixed && I % 25 == 0)
            {
                auto* Next = Expression<UMaterialExpressionCustom>(X, Y);
                Next->Inputs.SetNum(8); Next->OutputType = CMOT_Float1;
                Next->Code = TEXT("return I0 + I1;");
                for (int32 P = 0; P < Next->Inputs.Num(); ++P)
                {
                    Next->Inputs[P].InputName = FName(*FString::Printf(TEXT("I%d"), P));
                    Next->Inputs[P].Input.Connect(0, P ? Constant : Previous);
                }
                Previous = Next;
            }
            else if (bMixed && I % 10 == 0)
            {
                auto* Next = Expression<UMaterialExpressionMaterialFunctionCall>(X, Y);
                Next->SetMaterialFunction(CalledFunction.Get()); Next->FunctionInputs[0].Input.Connect(0, Previous); Previous = Next;
            }
            else if (bMixed && I % 3 == 0)
            {
                auto* Next = Expression<UMaterialExpressionMultiply>(X, Y);
                Next->A.Connect(0, Previous); Next->B.Connect(0, Constant); Previous = Next;
            }
            else
            {
                auto* Next = Expression<UMaterialExpressionAdd>(X, Y);
                Next->A.Connect(0, Previous); Next->B.Connect(0, Constant); Previous = Next; Add = Next;
            }
            if (bMixed && I % 40 == 1) { Previous->bCollapsed = false; }
        }
        if (bMixed)
        {
            auto* Comment = NewObject<UMaterialExpressionComment>(Material.Get(), NAME_None, RF_Transactional);
            Comment->Text = TEXT("Mixed expressions, shared inputs, function calls and previews");
            Comment->MaterialExpressionEditorX = -2200; Comment->MaterialExpressionEditorY = -1200;
            Comment->SizeX = 5000; Comment->SizeY = 2400 + (Count / 5) * 270;
            Material->GetExpressionCollection().AddComment(Comment);
        }
        if (Function)
        {
            Output = Expression<UMaterialExpressionFunctionOutput>(-1400, -200); Output->A.Connect(0, Previous);
            Output->SortPriority = 10;
            if (Family >= 2) { Output->A.Connect(0, Expression<UMaterialExpressionMakeMaterialAttributes>(-400, -500)); }
        }
        else { Material->GetExpressionInputForProperty(MP_EmissiveColor)->Connect(0, Previous); }
        Graph = NewObject<UMaterialGraph>(Material.Get(), NAME_None, RF_Transactional);
        Graph->Schema = UMaterialGraphSchema::StaticClass(); Graph->Material = Material.Get(); Graph->MaterialFunction = Function.Get();
        Material->MaterialGraph = Graph; Rebuild();
    }

    template<typename T> T* Expression(int32 X, int32 Y)
    {
        auto* Result = CastChecked<T>(UMaterialEditingLibrary::CreateMaterialExpressionEx(Material.Get(), Function.Get(), T::StaticClass(), nullptr, X, Y));
        if (Function) { Function->GetExpressionCollection().AddExpression(Result); }
        Result->bCollapsed = true; return Result;
    }
    void Rebuild() { Graph->RebuildGraph(); }
};
}
#endif
