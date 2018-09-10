#include "calc_fstr.h"
#include "feature_str.h"
#include "shap_values.h"
#include "util.h"

#include <catboost/libs/algo/index_calcer.h>
#include <catboost/libs/algo/quantization.h>
#include <catboost/libs/algo/learn_context.h>

#include <util/generic/algorithm.h>
#include <util/generic/maybe.h>
#include <util/generic/scope.h>
#include <util/generic/set.h>
#include <util/generic/xrange.h>

static TFeature GetFeature(const TModelSplit& split) {
    TFeature result;
    result.Type = split.Type;
    switch(result.Type) {
        case ESplitType::FloatFeature:
            result.FeatureIdx = split.FloatFeature.FloatFeature;
            break;
        case ESplitType::OneHotFeature:
            result.FeatureIdx = split.OneHotFeature.CatFeatureIdx;
            break;
        case ESplitType::OnlineCtr:
            result.Ctr = split.OnlineCtr.Ctr;
            break;
    }
    return result;
}

struct TFeatureHash {
    size_t operator()(const TFeature& f) const {
        return f.GetHash();
    }
};

static TVector<TMxTree> BuildTrees(const THashMap<TFeature, int, TFeatureHash>& featureToIdx,
                                   const TFullModel& model) {
    TVector<TMxTree> trees(model.ObliviousTrees.GetTreeCount());
    auto& binFeatures = model.ObliviousTrees.GetBinFeatures();
    for (int treeIdx = 0; treeIdx < trees.ysize(); ++treeIdx) {
        auto& tree = trees[treeIdx];
        const int leafCount = (1uLL << model.ObliviousTrees.TreeSizes[treeIdx]);

        tree.Leaves.resize(leafCount);
        for (int leafIdx = 0; leafIdx < leafCount; ++leafIdx) {
            tree.Leaves[leafIdx].Vals.resize(model.ObliviousTrees.ApproxDimension);
        }
        auto firstTreeLeafPtr = model.ObliviousTrees.GetFirstLeafPtrForTree(treeIdx);
        for (int leafIdx = 0; leafIdx < leafCount; ++leafIdx) {
            for (int dim = 0; dim < model.ObliviousTrees.ApproxDimension; ++dim) {
                tree.Leaves[leafIdx].Vals[dim] = firstTreeLeafPtr[leafIdx * model.ObliviousTrees.ApproxDimension + dim];
            }
        }
        auto treeSplitsStart = model.ObliviousTrees.TreeStartOffsets[treeIdx];
        auto treeSplitsStop = treeSplitsStart + model.ObliviousTrees.TreeSizes[treeIdx];
        for (auto splitIdx = treeSplitsStart; splitIdx < treeSplitsStop; ++splitIdx) {
            auto feature = GetFeature(binFeatures[model.ObliviousTrees.TreeSplits[splitIdx]]);
            tree.SrcFeatures.push_back(featureToIdx.at(feature));
        }
    }
    return trees;
}

TVector<TMxTree> BuildMatrixnetTrees(const TFullModel& model, TVector<TFeature>* features) {
    THashMap<TFeature, int, TFeatureHash> featureToIdx;
    const auto& modelBinFeatures = model.ObliviousTrees.GetBinFeatures();
    for (auto binSplit : model.ObliviousTrees.TreeSplits) {
        TFeature feature = GetFeature(modelBinFeatures[binSplit]);
        if (featureToIdx.has(feature)) {
            continue;
        }
        int featureIdx = featureToIdx.ysize();
        featureToIdx[feature] = featureIdx;
        features->push_back(feature);
    }

    return BuildTrees(featureToIdx, model);
}

TVector<std::pair<double, TFeature>> CalcFeatureEffect(const TFullModel& model, const TPool* pool) {
    if (model.GetTreeCount() == 0) {
        return TVector<std::pair<double, TFeature>>();
    }

    // use only if model.ObliviousTrees.LeafWeights is empty
    TVector<TVector<double>> leavesStatisticsOnPool;
    if (model.ObliviousTrees.LeafWeights.empty()) {
        CB_ENSURE(pool,
                  "CalcFeatureEffect requires either non-empty LeafWeights in model"
                  " or provided dataset");
        CB_ENSURE(pool->Docs.GetDocCount() != 0, "no docs in pool");
        CB_ENSURE(pool->Docs.GetEffectiveFactorCount() > 0, "no features in pool");

        leavesStatisticsOnPool = CollectLeavesStatistics(*pool, model);
    }

    TVector<TFeature> features;
    TVector<TMxTree> trees = BuildMatrixnetTrees(model, &features);

    TVector<double> effect = CalcEffect(trees,
                                        model.ObliviousTrees.LeafWeights.empty() ?
                                        leavesStatisticsOnPool : model.ObliviousTrees.LeafWeights);

    TVector<std::pair<double, int>> effectWithFeature;
    for (int i = 0; i < effect.ysize(); ++i) {
        effectWithFeature.emplace_back(effect[i], i);
    }
    Sort(effectWithFeature.begin(), effectWithFeature.end(), std::greater<std::pair<double, int>>());

    TVector<std::pair<double, TFeature>> result;
    for (int i = 0; i < effectWithFeature.ysize(); ++i) {
        result.emplace_back(effectWithFeature[i].first, features[effectWithFeature[i].second]);
    }
    return result;
}

TVector<TFeatureEffect> CalcRegularFeatureEffect(const TVector<std::pair<double, TFeature>>& internalEffect,
                                                 int catFeaturesCount, int floatFeaturesCount) {
    TVector<double> catFeatureEffect(catFeaturesCount);
    TVector<double> floatFeatureEffect(floatFeaturesCount);

    for (const auto& effectWithSplit : internalEffect) {
        TFeature feature = effectWithSplit.second;
        switch (feature.Type) {
            case ESplitType::FloatFeature:
                floatFeatureEffect[feature.FeatureIdx] += effectWithSplit.first;
                break;
            case ESplitType::OneHotFeature:
                catFeatureEffect[feature.FeatureIdx] += effectWithSplit.first;
                break;
            case ESplitType::OnlineCtr:
                auto& proj = feature.Ctr.Base.Projection;
                int featuresInSplit = proj.BinFeatures.ysize() + proj.CatFeatures.ysize() + proj.OneHotFeatures.ysize();
                double addEffect = effectWithSplit.first / featuresInSplit;
                for (const auto& binFeature : proj.BinFeatures) {
                    floatFeatureEffect[binFeature.FloatFeature] += addEffect;
                }
                for (auto catIndex : proj.CatFeatures) {
                    catFeatureEffect[catIndex] += addEffect;
                }
                for (auto oneHotFeature : proj.OneHotFeatures) {
                    catFeatureEffect[oneHotFeature.CatFeatureIdx] += addEffect;
                }
                break;
        }
    }

    double totalCat = Accumulate(catFeatureEffect.begin(), catFeatureEffect.end(), 0.0);
    double totalFloat = Accumulate(floatFeatureEffect.begin(), floatFeatureEffect.end(), 0.0);
    double total = totalCat + totalFloat;

    TVector<TFeatureEffect> regularFeatureEffect;
    for (int i = 0; i < catFeatureEffect.ysize(); ++i) {
        if (catFeatureEffect[i] > 0) {
            regularFeatureEffect.push_back(TFeatureEffect(catFeatureEffect[i] / total * 100, EFeatureType::Categorical, i));
        }
    }
    for (int i = 0; i < floatFeatureEffect.ysize(); ++i) {
        if (floatFeatureEffect[i] > 0) {
            regularFeatureEffect.push_back(TFeatureEffect(floatFeatureEffect[i] / total * 100, EFeatureType::Float, i));
        }
    }

    Sort(regularFeatureEffect.rbegin(), regularFeatureEffect.rend(), [](const TFeatureEffect& left, const TFeatureEffect& right) {
        return left.Score < right.Score;
    });
    return regularFeatureEffect;
}

TVector<double> CalcRegularFeatureEffect(const TFullModel& model, const TPool* pool) {
    NCB::TFeaturesLayout layout(model.ObliviousTrees.FloatFeatures, model.ObliviousTrees.CatFeatures);

    TVector<TFeatureEffect> regularEffect = CalcRegularFeatureEffect(CalcFeatureEffect(model, pool),
                                                                     model.GetNumCatFeatures(),
                                                                     model.GetNumFloatFeatures());

    TVector<double> effect(layout.GetExternalFeatureCount());
    for (const auto& featureEffect : regularEffect) {
        int externalFeatureIdx = layout.GetExternalFeatureIdx(featureEffect.Feature.Index,
                                                              featureEffect.Feature.Type);
        effect[externalFeatureIdx] = featureEffect.Score;
    }

    return effect;
}

TVector<TInternalFeatureInteraction> CalcInternalFeatureInteraction(const TFullModel& model) {
    if (model.GetTreeCount() == 0) {
        return TVector<TInternalFeatureInteraction>();
    }

    TVector<TFeature> features;
    TVector<TMxTree> trees = BuildMatrixnetTrees(model, &features);

    TVector<TFeaturePairInteractionInfo> pairwiseEffect = CalcMostInteractingFeatures(trees);
    TVector<TInternalFeatureInteraction> result;
    result.reserve(pairwiseEffect.size());
    for (const auto& efffect : pairwiseEffect) {
        result.emplace_back(efffect.Score, features[efffect.Feature1], features[efffect.Feature2]);
    }
    return result;
}

TVector<TFeatureInteraction> CalcFeatureInteraction(const TVector<TInternalFeatureInteraction>& internalFeatureInteraction,
                                                          const NCB::TFeaturesLayout& layout) {
    THashMap<std::pair<int, int>, double> sumInteraction;
    double totalEffect = 0;

    for (const auto& effectWithFeaturePair : internalFeatureInteraction) {
        TVector<TFeature> features{effectWithFeaturePair.FirstFeature, effectWithFeaturePair.SecondFeature};

        TVector<TVector<int>> internalToRegular;
        for (const auto& internalFeature : features) {
            TVector<int> regularFeatures;
            if (internalFeature.Type == ESplitType::FloatFeature) {
                regularFeatures.push_back(layout.GetExternalFeatureIdx(internalFeature.FeatureIdx, EFeatureType::Float));
            } else {
                auto proj = internalFeature.Ctr.Base.Projection;
                for (auto& binFeature : proj.BinFeatures) {
                    regularFeatures.push_back(layout.GetExternalFeatureIdx(binFeature.FloatFeature, EFeatureType::Float));
                }
                for (auto catFeature : proj.CatFeatures) {
                    regularFeatures.push_back(layout.GetExternalFeatureIdx(catFeature, EFeatureType::Categorical));
                }
            }
            internalToRegular.push_back(regularFeatures);
        }

        double effect = effectWithFeaturePair.Score;
        for (int f0 : internalToRegular[0]) {
            for (int f1 : internalToRegular[1]) {
                if (f0 == f1) {
                    continue;
                }
                if (f1 < f0) {
                    DoSwap(f0, f1);
                }
                sumInteraction[std::make_pair(f0, f1)] += effect / (internalToRegular[0].ysize() * internalToRegular[1].ysize());
            }
        }
        totalEffect += effect;
    }

    TVector<TFeatureInteraction> regularFeatureEffect;
    for (const auto& pairInteraction : sumInteraction) {
        int f0 = pairInteraction.first.first;
        int f1 = pairInteraction.first.second;
        regularFeatureEffect.push_back(
            TFeatureInteraction(sumInteraction[pairInteraction.first] / totalEffect * 100,
                                layout.GetExternalFeatureType(f0),
                                layout.GetInternalFeatureIdx(f0),
                                layout.GetExternalFeatureType(f1),
                                layout.GetInternalFeatureIdx(f1)));
    }

    Sort(regularFeatureEffect.rbegin(), regularFeatureEffect.rend(), [](const TFeatureInteraction& left, const TFeatureInteraction& right) {
        return left.Score < right.Score;
    });
    return regularFeatureEffect;
}

TString TFeature::BuildDescription(const NCB::TFeaturesLayout& layout) const {
    TStringBuilder result;
    if (Type == ESplitType::OnlineCtr) {
        result << ::BuildDescription(layout, Ctr.Base.Projection);
        result << " prior_num=" << Ctr.PriorNum;
        result << " prior_denom=" << Ctr.PriorDenom;
        result << " targetborder=" << Ctr.TargetBorderIdx;
        result << " type=" << Ctr.Base.CtrType;
    } else if (Type == ESplitType::FloatFeature) {
        result << BuildFeatureDescription(layout, FeatureIdx, EFeatureType::Float);
    } else {
        Y_ASSERT(Type == ESplitType::OneHotFeature);
        result << BuildFeatureDescription(layout, FeatureIdx, EFeatureType::Categorical);
    }
    return result;
}

static TVector<TVector<double>> CalcFstr(const TFullModel& model, const TPool* pool){
    CB_ENSURE(!model.ObliviousTrees.LeafWeights.empty() || (pool != nullptr),
              "CalcFstr requires either non-empty LeafWeights in model or provided dataset");

    TVector<double> regularEffect = CalcRegularFeatureEffect(model, pool);
    TVector<TVector<double>> result;
    for (const auto& value : regularEffect){
        TVector<double> vec = {value};
        result.push_back(vec);
    }
    return result;
}

TVector<TVector<double>> CalcInteraction(const TFullModel& model){
    NCB::TFeaturesLayout layout(model.ObliviousTrees.FloatFeatures, model.ObliviousTrees.CatFeatures);

    TVector<TInternalFeatureInteraction> internalInteraction = CalcInternalFeatureInteraction(model);
    TVector<TFeatureInteraction> interaction = CalcFeatureInteraction(internalInteraction, layout);
    TVector<TVector<double>> result;
    for (const auto& value : interaction){
        int featureIdxFirst = layout.GetExternalFeatureIdx(value.FirstFeature.Index,
                                                           value.FirstFeature.Type);
        int featureIdxSecond = layout.GetExternalFeatureIdx(value.SecondFeature.Index,
                                                            value.SecondFeature.Type);
        TVector<double> vec = {static_cast<double>(featureIdxFirst), static_cast<double>(featureIdxSecond), value.Score};
        result.push_back(vec);
    }
    return result;
}


static bool AllFeatureIdsEmpty(const TVector<TString>& featureIds) {
    return AllOf(featureIds.begin(),
                 featureIds.end(),
                 [](const auto& id) { return id.empty(); });
}


TVector<TVector<double>> GetFeatureImportances(const TString& type,
                                               const TFullModel& model,
                                               const TPool* pool,
                                               int threadCount,
                                               int logPeriod) {
    SetVerboseLogingMode();
    Y_SCOPE_EXIT() { SetSilentLogingMode(); };

    EFstrType FstrType = FromString<EFstrType>(type);

    switch (FstrType) {
        case EFstrType::FeatureImportance:
            return CalcFstr(model, pool);
        case EFstrType::Interaction:
            return CalcInteraction(model);
        case EFstrType::ShapValues: {
            CB_ENSURE(pool, "dataset is not provided");

            NPar::TLocalExecutor localExecutor;
            localExecutor.RunAdditionalThreads(threadCount - 1);

            return CalcShapValues(model, *pool, &localExecutor, logPeriod);
        }
        default:
            Y_UNREACHABLE();
    }
}

TVector<TVector<TVector<double>>> GetFeatureImportancesMulti(const TString& type,
                                                             const TFullModel& model,
                                                             const TPool* pool,
                                                             int threadCount,
                                                             int logPeriod) {
    SetVerboseLogingMode();
    Y_SCOPE_EXIT() { SetSilentLogingMode(); };

    EFstrType FstrType = FromString<EFstrType>(type);

    CB_ENSURE(FstrType == EFstrType::ShapValues, "Only shap values can provide multi approxes.");

    CB_ENSURE(pool, "dataset is not provided");

    NPar::TLocalExecutor localExecutor;
    localExecutor.RunAdditionalThreads(threadCount - 1);

    return CalcShapValuesMulti(model, *pool, &localExecutor, logPeriod);
}

TVector<TString> GetMaybeGeneratedModelFeatureIds(const TFullModel& model, const TPool* pool)
{
    NCB::TFeaturesLayout layout(model.ObliviousTrees.FloatFeatures, model.ObliviousTrees.CatFeatures);
    TVector<TString> modelFeatureIds = layout.GetExternalFeatureIds();
    if (AllFeatureIdsEmpty(modelFeatureIds)) {
        if (pool && !AllFeatureIdsEmpty(pool->FeatureId)) {
            CB_ENSURE(pool->FeatureId.size() >= (size_t)layout.GetExternalFeatureCount(), "dataset has less features than the model");
            modelFeatureIds.assign(pool->FeatureId.begin(), pool->FeatureId.begin() + layout.GetExternalFeatureCount());
        }
    }
    for (size_t i = 0; i < modelFeatureIds.size(); ++i) {
        if (modelFeatureIds[i].empty()) {
            modelFeatureIds[i] = ToString(i);
        }
    }
    return modelFeatureIds;
}

