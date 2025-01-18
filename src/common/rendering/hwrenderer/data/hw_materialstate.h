
#pragma once

#include "textures.h"

class FMaterial;

struct FMaterialState
{
	FMaterial* mMaterial = nullptr;
	int mClampMode = 0;
	int mTranslation = CLAMP_NONE;
	int mOverrideShader = -1;
	bool mChanged = false;
	GlobalShaderAddr globalShaderAddr = {0, 3, 0};
	MaterialLayerSampling mOverrideFilter = MaterialLayerSampling::Default; // override filter for non-custom layers

	void Reset()
	{
		mMaterial = nullptr;
		mTranslation = 0;
		mClampMode = CLAMP_NONE;
		mOverrideShader = -1;
		mChanged = false;
		globalShaderAddr = {0, 3, 0};
		mOverrideFilter = MaterialLayerSampling::Default;
	}
};
