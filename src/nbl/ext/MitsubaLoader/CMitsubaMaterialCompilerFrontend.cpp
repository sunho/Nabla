// Copyright (C) 2018-2020 - DevSH Graphics Programming Sp. z O.O.
// This file is part of the "Nabla Engine".
// For conditions of distribution and use, see copyright notice in nabla.h

#include "nbl/ext/MitsubaLoader/CMitsubaMaterialCompilerFrontend.h"
#include "nbl/ext/MitsubaLoader/SContext.h"

namespace nbl::ext::MitsubaLoader
{

auto CMitsubaMaterialCompilerFrontend::getTexture(const ext::MitsubaLoader::SContext* _loaderContext, const CElementTexture* _element, const E_IMAGE_VIEW_SEMANTIC semantic) -> tex_ass_type
{
    // first unwind the texture Scales
    float scale = 1.f;
    while (_element && _element->type==CElementTexture::SCALE)
    {
        scale *= _element->scale.scale;
        _element = _element->scale.texture;
    }
    _NBL_DEBUG_BREAK_IF(_element && _element->type!=CElementTexture::BITMAP);
    if (!_element)
    {
        os::Printer::log("[ERROR] Could Not Find Texture, dangling reference after scale unroll, substituting 2x2 Magenta Checkerboard Error Texture.", ELL_ERROR);
        return getErrorTexture(_loaderContext);
    }

    asset::IAsset::E_TYPE types[2]{ asset::IAsset::ET_IMAGE_VIEW, asset::IAsset::ET_TERMINATING_ZERO };
    const auto key = _loaderContext->imageViewCacheKey(_element->bitmap,semantic);
    auto viewBundle = _loaderContext->override_->findCachedAsset(key,types,_loaderContext->inner,0u);
    if (!viewBundle.getContents().empty())
    {
        auto view = core::smart_refctd_ptr_static_cast<asset::ICPUImageView>(viewBundle.getContents().begin()[0]);

        // TODO: here for the bumpmap bug
        auto found = _loaderContext->derivMapCache.find(view->getCreationParameters().image);
        if (found!=_loaderContext->derivMapCache.end())
        {
            const float normalizationFactor = found->second;
            scale *= normalizationFactor;
        }

        types[0] = asset::IAsset::ET_SAMPLER;
        const std::string samplerKey = _loaderContext->samplerCacheKey(_loaderContext->computeSamplerParameters(_element->bitmap));
        auto samplerBundle = _loaderContext->override_->findCachedAsset(samplerKey, types, _loaderContext->inner, 0u);
        assert(!samplerBundle.getContents().empty());
        auto sampler = core::smart_refctd_ptr_static_cast<asset::ICPUSampler>(samplerBundle.getContents().begin()[0]);

        return {view, sampler, scale};
    }
    return { nullptr, nullptr, core::nan<float>()};
}

auto CMitsubaMaterialCompilerFrontend::getErrorTexture(const ext::MitsubaLoader::SContext* _loaderContext) -> tex_ass_type
{
    constexpr const char* ERR_TEX_CACHE_NAME = "nbl/builtin/image_view/dummy2d";
    constexpr const char* ERR_SMPLR_CACHE_NAME = "nbl/builtin/sampler/default";

    asset::IAsset::E_TYPE types[2]{ asset::IAsset::ET_IMAGE_VIEW, asset::IAsset::ET_TERMINATING_ZERO };
    auto bundle = _loaderContext->override_->findCachedAsset(ERR_TEX_CACHE_NAME, types, _loaderContext->inner, 0u);
    assert(!bundle.getContents().empty()); // this shouldnt ever happen since ERR_TEX_CACHE_NAME is builtin asset
        
    auto view = core::smart_refctd_ptr_static_cast<asset::ICPUImageView>(bundle.getContents().begin()[0]);

    types[0] = asset::IAsset::ET_SAMPLER;
    auto sbundle = _loaderContext->override_->findCachedAsset(ERR_SMPLR_CACHE_NAME, types, _loaderContext->inner, 0u);
    assert(!sbundle.getContents().empty()); // this shouldnt ever happen since ERR_SMPLR_CACHE_NAME is builtin asset

    auto smplr = core::smart_refctd_ptr_static_cast<asset::ICPUSampler>(sbundle.getContents().begin()[0]);

    return { view, smplr, 1.f };
}

auto CMitsubaMaterialCompilerFrontend::createIRNode(SContext& ctx, const CElementBSDF* _bsdf) -> node_handle_t
{
    using namespace asset::material_compiler;

    auto transparent = [](const float eta) -> bool {return eta>0.99999f&&eta<1.000001f;};

    auto getFloatOrTexture = [&ctx](const CElementTexture::FloatOrTexture& src) -> IR::INode::SParameter<float>
    {
        if (src.value.type == SPropertyElementData::INVALID)
        {
            IR::INode::STextureSource tex;
            std::tie(tex.image, tex.sampler, tex.scale) = getTexture(ctx.m_loaderContext,src.texture);
            return tex;
        }
        else
            return src.value.fvalue;
    };
    auto getSpectrumOrTexture = [&ctx](const CElementTexture::SpectrumOrTexture& src, const E_IMAGE_VIEW_SEMANTIC semantic=EIVS_IDENTITIY) -> IR::INode::SParameter<IR::INode::color_t>
    {
        if (src.value.type == SPropertyElementData::INVALID)
        {
            IR::INode::STextureSource tex;
            std::tie(tex.image, tex.sampler, tex.scale) = getTexture(ctx.m_loaderContext,src.texture,semantic);
            return tex;
        }
        else
            return src.value.vvalue;
    };

    auto setAlpha = [&getFloatOrTexture](IR::IMicrofacetBSDFNode* node, const bool rough, const auto& _bsdfEl) -> void
    {
        if (rough)
        {
            using bsdf_t = std::remove_const_t<std::remove_reference_t<decltype(_bsdfEl)>>;
            if constexpr (std::is_same_v<bsdf_t,CElementBSDF::AllDiffuse> || std::is_same_v<bsdf_t,CElementBSDF::DiffuseTransmitter>)
                getFloatOrTexture(_bsdfEl.alpha,node->alpha_u);
            else
                getFloatOrTexture(_bsdfEl.alphaU,node->alpha_u);
            node->alpha_v = node->alpha_u;
            if constexpr (std::is_base_of_v<CElementBSDF::RoughSpecularBase,bsdf_t>)
            {
                constexpr IR::CMicrofacetSpecularBSDFNode::E_NDF ndfMap[4] =
                {
                    IR::CMicrofacetSpecularBSDFNode::ENDF_BECKMANN,
                    IR::CMicrofacetSpecularBSDFNode::ENDF_GGX,
                    IR::CMicrofacetSpecularBSDFNode::ENDF_PHONG,
                    IR::CMicrofacetSpecularBSDFNode::ENDF_ASHIKHMIN_SHIRLEY
                };
                auto& ndf = static_cast<IR::ICookTorranceBSDFNode*>(node)->ndf;
                ndf = ndfMap[_bsdfEl.distribution];
                if (ndf==IR::CMicrofacetSpecularBSDFNode::ENDF_ASHIKHMIN_SHIRLEY)
                    getFloatOrTexture(_bsdfEl.alphaV,node->alpha_v);
            }
        }
        else
            node->setSmooth();
    };

    auto ir_node = IR::invalid_node;
    auto& hashCons = ctx.m_hashCons;
    auto findChild = [&hashCons,_bsdf](const uint32_t childIx) -> node_handle_t
    {
        return std::get<node_handle_t>(*hashCons.find(_bsdf->meta_common.bsdf[0]));
    };
    auto* ir = ctx.m_ir;
    const auto type = _bsdf->type;
    switch (type)
    {
        case CElementBSDF::TWO_SIDED:
            //TWO_SIDED is not translated into IR node directly
            break;
        case CElementBSDF::MASK:
        {
            IR::INode::SParameter<IR::INode::color_t> opacity = getSpectrumOrTexture(_bsdf->mask.opacity,EIVS_BLEND_WEIGHT);
            // TODO: optimize out full transparent or full solid
            if (true) // not transparent
            {
                auto child = findChild(0u);
                if (true) // not solid
                {
                    ir_node = ir->allocNode<IR::COpacityNode>(1);
                    auto pNode = ir->getNode<IR::COpacityNode>(ir_node);
                    pNode->opacity = std::move(opacity);
                    pNode->getChildrenArray()[0] = child;
                }
                else
                    ir_node = child;
            }
            else
                ir_node = {0xdeadbeefu};//fullyTransparentIRNode;
            break;
        }
#if 0
        case CElementBSDF::DIFFUSE:
        case CElementBSDF::ROUGHDIFFUSE:
        {
            assert(!childCount && firstChild==0xdeadbeefu);
            ir_node = ir->allocNode<IR::CMicrofacetDiffuseBSDFNode>();
            auto pNode = ir->getNode<IR::CMicrofacetDiffuseBSDFNode>(ir_node);
            setAlpha(pNode,type==CElementBSDF::ROUGHDIFFUSE,_bsdf->diffuse);

            getSpectrumOrTexture(_bsdf->diffuse.reflectance,pNode->reflectance);
            break;
        }
        case CElementBSDF::CONDUCTOR:
        case CElementBSDF::ROUGHCONDUCTOR:
        {
            assert(!childCount && firstChild==0xdeadbeefu);
            ir_node = ir->allocNode<IR::CMicrofacetSpecularBSDFNode>();
            auto pNode = ir->getNode<IR::CMicrofacetSpecularBSDFNode>(ir_node);
            setAlpha(pNode,type==CElementBSDF::ROUGHCONDUCTOR,_bsdf->conductor);
            const float extEta = _bsdf->conductor.extEta;
            pNode->eta = _bsdf->conductor.eta.vvalue/extEta;
            pNode->etaK = _bsdf->conductor.k.vvalue/extEta;
            break;
        }
        case CElementBSDF::DIFFUSE_TRANSMITTER:
        {
            assert(!childCount && firstChild==0xdeadbeefu);
            ir_node = ir->allocNode<IR::CMicrofacetDifftransBSDFNode>();
            auto pNode = ir->getNode<IR::CMicrofacetDifftransBSDFNode>(ir_node);
            pNode->setSmooth();
            getSpectrumOrTexture(_bsdf->difftrans.transmittance,pNode->transmittance);
            break;
        }
        case CElementBSDF::PLASTIC:
        case CElementBSDF::ROUGHPLASTIC:
        {
            assert(childCount==1);
            const bool rough = type==CElementBSDF::ROUGHPLASTIC;

            const float eta = _bsdf->plastic.intIOR/_bsdf->plastic.extIOR;
            if (transparent(eta))
            {
                os::Printer::log("WARNING: Dielectric with IoR=1.0!", _bsdf->id, ELL_WARNING);
                ir_node = firstChild;
            }
            else
            {
                ir_node = ir->allocNode<IR::CMicrofacetCoatingBSDFNode>();
                auto coat = ir->getNode<IR::CMicrofacetCoatingBSDFNode>(ir_node);
                setAlpha(coat,rough,_bsdf->plastic);
                coat->eta = IR::INode::color_t(eta);
            }

            auto* coated = ir->getNode(firstChild);
            auto* node_diffuse = dynamic_cast<IR::CMicrofacetDiffuseBSDFNode*>(coated);
            setAlpha(node_diffuse,rough,_bsdf->plastic);

            getSpectrumOrTexture(_bsdf->plastic.diffuseReflectance,node_diffuse->reflectance);
            break;
        }
        case CElementBSDF::DIELECTRIC:
        case CElementBSDF::THINDIELECTRIC:
        case CElementBSDF::ROUGHDIELECTRIC:
        {
            assert(!childCount && firstChild==0xdeadbeefu);
            const float eta = _bsdf->dielectric.intIOR/_bsdf->dielectric.extIOR;
            if (transparent(eta))
            {
                os::Printer::log("WARNING: Dielectric with IoR=1.0!", _bsdf->id, ELL_WARNING);
                ir_node = ir->allocNode<IR::COpacityNode>();
                auto pNode = ir->getNode<IR::COpacityNode>(ir_node);
                pNode->opacity = IR::INode::color_t(0.f);
            }
            else
            {
                ir_node = ir->allocNode<IR::CMicrofacetDielectricBSDFNode>();
                auto dielectric = ir->getNode<IR::CMicrofacetDielectricBSDFNode>(ir_node);
                setAlpha(dielectric,type==CElementBSDF::ROUGHDIELECTRIC,_bsdf->dielectric);
                dielectric->eta = IR::INode::color_t(eta);
                dielectric->thin = type==CElementBSDF::THINDIELECTRIC;
            }
            break;
        }
        case CElementBSDF::BUMPMAP:
        {
            assert(childCount==1);
            ir_node = ir->allocNode<IR::CGeomModifierNode>(IR::CGeomModifierNode::ET_DERIVATIVE);
            auto* pNode = ir->getNode<IR::CGeomModifierNode>(ir_node);
            //no other source supported for now (uncomment in the future) [far future TODO]
            //node->source = IR::CGeomModifierNode::ESRC_TEXTURE;

            std::tie(pNode->texture.image,pNode->texture.sampler,pNode->texture.scale) =
                getTexture(_bsdf->bumpmap.texture,_bsdf->bumpmap.wasNormal ? EIVS_NORMAL_MAP:EIVS_BUMP_MAP);
            break;
        }
        case CElementBSDF::COATING:
        case CElementBSDF::ROUGHCOATING:
        {
            assert(childCount==1);
            const bool rough = type==CElementBSDF::ROUGHDIELECTRIC;

            const float eta = _bsdf->dielectric.intIOR/_bsdf->dielectric.extIOR;
            if (transparent(eta))
                ir_node = firstChild;
            else
            {
                ir_node = ir->allocNode<IR::CMicrofacetCoatingBSDFNode>();
                auto* node = ir->getNode<IR::CMicrofacetCoatingBSDFNode>(ir_node);
                setAlpha(node,rough,_bsdf->coating);

                node->eta = IR::INode::color_t(eta);

                const float thickness = _bsdf->coating.thickness;
                getSpectrumOrTexture(_bsdf->coating.sigmaA,node->thicknessSigmaA);
                if (node->thicknessSigmaA.isConstant())
                    node->thicknessSigmaA.constant *= thickness;
                else
                    node->thicknessSigmaA.texture.scale *= thickness;
            }
            auto* coated = ir->getNode(firstChild);
            auto* node_diffuse = dynamic_cast<IR::CMicrofacetDiffuseBSDFNode*>(coated);
            setAlpha(node_diffuse,rough,_bsdf->coating);
            break;
        }
        break;
        case CElementBSDF::BLEND_BSDF:
        {
            assert(childCount==2);
            ir_node = ir->allocNode<IR::CBSDFBlendNode>(firstChild);
            auto* node = ir->getNode<IR::CBSDFBlendNode>(ir_node);
            if (_bsdf->blendbsdf.weight.value.type == SPropertyElementData::INVALID)
            {
                std::tie(node->weight.texture.image, node->weight.texture.sampler, node->weight.texture.scale) =
                    getTexture(_bsdf->blendbsdf.weight.texture,EIVS_BLEND_WEIGHT);
                assert(!core::isnan(node->weight.texture.scale));
            }
            else
                node->weight = IR::INode::color_t(_bsdf->blendbsdf.weight.value.fvalue);
        }
        break;
        case CElementBSDF::MIXTURE_BSDF:
        {
            assert(childCount>1);
            ir_node = ir->allocNode<IR::CBSDFMixNode>(firstChild,childCount);
            auto* node = ir->getNode<IR::CBSDFMixNode>(ir_node);
            const auto* weightIt = _bsdf->mixturebsdf.weights;
            for (size_t i=0u; i<cnt; i++)
                node->weights[i] = *(weightIt++);
        }
        break;
#endif
        default:
            break;
    }
    assert(ir_node!=IR::invalid_node);
    return ir_node;
}

auto CMitsubaMaterialCompilerFrontend::compileToIRTree(SContext& ctx, const CElementBSDF* _root) -> front_and_back_t
{
    using namespace asset;
    using namespace material_compiler;

    auto unwindTwosided = [](const CElementBSDF* _bsdf) -> const CElementBSDF*
    {
        while (_bsdf->type==CElementBSDF::TWO_SIDED)
            _bsdf = _bsdf->meta_common.bsdf[0];
        return _bsdf;
    };

    struct DFSData
    {
        const CElementBSDF* bsdf;
        uint16_t visited : 1;
        /*
        IRNode* ir_node = nullptr;
        uint32_t parent_ix = static_cast<uint32_t>(-1);
        uint32_t child_num = 0u;
        bool twosided = false;
        bool front = true;
        */
        CElementBSDF::Type type() const { return bsdf->type; }
    };
    core::stack<DFSData> dfs;
    auto push = [&dfs](const CElementBSDF* _bsdf)
    {
        auto& el = dfs.emplace();
        el.bsdf = _bsdf;
        el.visited = false;
        //root.twosided = (root.type() == CElementBSDF::TWO_SIDED);
    };
    push(unwindTwosided(_root));
    while (!dfs.empty())
    {
        auto& parent = dfs.top();
        assert(parent.type()!=CElementBSDF::TWO_SIDED);
        assert(parent.bsdf->isMeta());

        if (parent.visited)
        {
            dfs.pop();
            createIRNode(ir,parent.bsdf);
        }
        else
        {
            parent.visited = true;

            const bool isCoating = parent.type()==CElementBSDF::COATING;
            const auto childCount = isCoating ? parent.bsdf->coating.childCount:parent.bsdf->meta_common.childCount;
            for (auto i=0u; i<childCount; i++)
            {
                // unwind twosided
                const auto originalChild = isCoating ? parent.bsdf->coating.bsdf[i]:parent.bsdf->meta_common.bsdf[i];
                auto child = unwindTwosided(originalChild);

                // check for meta
                if (child->isMeta())
                    push(child);
                else
                    createIRNode(ir,child,{0xdeadbeefu},0u);

                //child_node.parent_ix = isTwosidedMeta ? parent.parent_ix : bfs.size();
                //child_node.twosided = (child_node.type() == CElementBSDF::TWO_SIDED) || parent.twosided;
                //child_node.child_num = isTwosidedMeta ? parent.child_num : i;
                //child_node.front = parent.front;
                //if (parent.type() == CElementBSDF::TWO_SIDED && i == 1u)
                    //child_node.front = false;
            }
        }
    }




    struct SNode
    {
        const CElementBSDF* bsdf;
        IRNode* ir_node = nullptr;
        uint32_t parent_ix = static_cast<uint32_t>(-1);
        uint32_t child_num = 0u;
        bool twosided = false;
        bool front = true;

        CElementBSDF::Type type() const { return bsdf->type; }
    };
    auto node_parent = [](const SNode& node, core::vector<SNode>& traversal)
    {
        return &traversal[node.parent_ix];
    };

    core::vector<SNode> bfs;
    {
        core::queue<SNode> q;
        {
            SNode root{ _bsdf };
            root.twosided = (root.type() == CElementBSDF::TWO_SIDED);
            q.push(root);
        }

        while (q.size())
        {
            SNode parent = q.front();
            q.pop();
            //node.ir_node = createIRNode(node.bsdf);

            if (parent.bsdf->isMeta())
            {
                const uint32_t child_count = (parent.bsdf->type == CElementBSDF::COATING) ? parent.bsdf->coating.childCount : parent.bsdf->meta_common.childCount;
                for (uint32_t i = 0u; i < child_count; ++i)
                {
                    SNode child_node;
                    child_node.bsdf = (parent.bsdf->type == CElementBSDF::COATING) ? parent.bsdf->coating.bsdf[i] : parent.bsdf->meta_common.bsdf[i];
                    child_node.parent_ix = parent.type() == CElementBSDF::TWO_SIDED ? parent.parent_ix : bfs.size();
                    child_node.twosided = (child_node.type() == CElementBSDF::TWO_SIDED) || parent.twosided;
                    child_node.child_num = (parent.type() == CElementBSDF::TWO_SIDED) ? parent.child_num : i;
                    child_node.front = parent.front;
                    if (parent.type() == CElementBSDF::TWO_SIDED && i == 1u)
                        child_node.front = false;
                    q.push(child_node);
                }
            }
            if (parent.type() != CElementBSDF::TWO_SIDED)
                bfs.push_back(parent);
        }
    }

    auto createBackfaceNodeFromFrontface = [&ir](const IRNode* front) -> IRNode*
    {
        switch (front->symbol)
        {
        case IRNode::ES_BSDF_COMBINER: [[fallthrough]];
        case IRNode::ES_OPACITY: [[fallthrough]];
        case IRNode::ES_GEOM_MODIFIER: [[fallthrough]];
        case IRNode::ES_EMISSION:
            return ir->copyNode(front);
        case IRNode::ES_BSDF:
        {
            auto* bsdf = static_cast<const IR::CBSDFNode*>(front);
            if (bsdf->type == IR::CBSDFNode::ET_MICROFACET_DIELECTRIC)
            {
                auto* dielectric = static_cast<const IR::CMicrofacetDielectricBSDFNode*>(bsdf);
                auto* copy = ir->copyNode<IR::CMicrofacetDielectricBSDFNode>(front);
                if (!copy->thin) //we're always outside in case of thin dielectric
                    copy->eta = IRNode::color_t(1.f) / copy->eta;

                return copy;
            }
            else if (bsdf->type == IR::CBSDFNode::ET_MICROFACET_DIFFTRANS)
                return ir->copyNode(front);
        }
        [[fallthrough]]; // intentional
        default:
        {
            // black diffuse otherwise
            auto* invalid = ir->allocNode<IR::CMicrofacetDiffuseBSDFNode>();
            invalid->setSmooth();
            invalid->reflectance = IR::INode::color_t(0.f);

            return invalid;
        }
        }
    };

    //create frontface IR
    IRNode* frontroot = nullptr;
    for (auto& node : bfs)
    {
        if (!node.front)
            continue;

        IRNode** dst = nullptr;
        if (node.parent_ix >= bfs.size())
            dst = &frontroot;
        else
            dst = const_cast<IRNode**>(&node_parent(node, bfs)->ir_node->children[node.child_num]);

        node.ir_node = *dst = createIRNode(ir, node.bsdf);
    }
    IRNode* backroot = nullptr;
    for (uint32_t i = 0u; i < bfs.size(); ++i)
    {
        SNode& node = bfs[i];

        IRNode* ir_node = nullptr;
        if (!node.twosided)
            ir_node = createBackfaceNodeFromFrontface(node.ir_node);
        else
        {
            if (node.front)
            {
                if ((i+1u) < bfs.size() && bfs[i+1u].twosided && !bfs[i+1u].front)
                    continue; // will take backface node in next iteration
                //otherwise copy the one from front (same bsdf on both sides_
                ir_node = ir->copyNode(node.ir_node);
            }
            else
                ir_node = createIRNode(ir, node.bsdf);
        }
        node.ir_node = ir_node;

        IRNode** dst = nullptr;
        if (node.parent_ix >= bfs.size())
            dst = &backroot;
        else
            dst = const_cast<IRNode**>(&node_parent(node, bfs)->ir_node->children[node.child_num]);

        *dst = ir_node;
    }

    ir->addRootNode(frontroot);
    ir->addRootNode(backroot);

    return { frontroot, backroot };
}

}