#pragma once

#include <string>

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

// A leaf (MeshQueues.h treeLeaf) is drawn with the Leaf shader whatever its alpha
// mode; only the rest of a tree is a trunk (Backstab's olive leaves: alpha mode 0).
inline bool treeTrunk(bool leaf, TreeMaterial kind)
{
	return !leaf && (kind == TreeMaterial::TrunkBase || kind == TreeMaterial::TrunkDetail);
}

// The overgrowth's distant "<geometry>_lod" meshes (RendDX9 0x10091972): a
// material whose texture name has "tile_" goes to the cell's TrunkOG batch,
// any other to its LeafOG batch, which always alpha-tests (AlphaRef 127),
// whatever the material's alpha mode.
inline TreeMaterial overgrowthMaterial(const std::string& texture)
{
	return texture.find("tile_") != std::string::npos ? TreeMaterial::TrunkBase : TreeMaterial::Leaf;
}

} // namespace dice::hfe::render
