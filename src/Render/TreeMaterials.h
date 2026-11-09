#pragma once

namespace dice::hfe::render {

// RendDX9 0x100bdb00: vegetation alpha-tested materials are leaves;
// its remaining materials take TrunkSTMBase or TrunkSTMDetail.
enum class TreeMaterial { Other, Leaf, TrunkBase, TrunkDetail };
inline TreeMaterial treeMaterial(bool vegetation, int alphaMode, bool hasDetail)
{
	if (!vegetation) return TreeMaterial::Other;
	if (alphaMode == 2) return TreeMaterial::Leaf;
	return hasDetail ? TreeMaterial::TrunkDetail : TreeMaterial::TrunkBase;
}

} // namespace dice::hfe::render
