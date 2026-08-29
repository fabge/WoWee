#include "ui/ui_upload_budget.hpp"
#include "ui/ui_texture_load.hpp"

#include "core/window.hpp"
#include "pipeline/asset_manager.hpp"
#include "pipeline/blp_loader.hpp"
#include "rendering/vk_context.hpp"
#include "pipeline/dbc_layout.hpp"
#include "core/logger.hpp"

namespace wowee::ui {

VkDescriptorSet uploadUiTextureFromBlp(pipeline::AssetManager* assetManager,
                                       const std::string& path,
                                       core::Window* window,
                                       UiTextureLoad* why) {
    const auto fail = [&](UiTextureLoad reason) {
        if (why) *why = reason;
        return VK_NULL_HANDLE;
    };

    if (!assetManager) return fail(UiTextureLoad::NotFound);

    auto blpData = assetManager->readFile(path);
    if (blpData.empty()) return fail(UiTextureLoad::NotFound);

    auto image = pipeline::BLPLoader::load(blpData);
    if (!image.isValid()) return fail(UiTextureLoad::DecodeFailed);

    auto* vkCtx = window ? window->getVkContext() : nullptr;
    if (!vkCtx) return fail(UiTextureLoad::NoContext);

    if (why) *why = UiTextureLoad::Ok;
    return vkCtx->uploadImGuiTexture(image.data.data(), image.width, image.height);
}


VkDescriptorSet cachedIconTexture(
    uint32_t iconId, pipeline::AssetManager* assetManager, core::Window* window,
    const std::unordered_map<uint32_t, std::string>& paths,
    std::unordered_map<uint32_t, VkDescriptorSet>& cache) {
    if (iconId == 0 || !assetManager) return VK_NULL_HANDLE;

    auto cit = cache.find(iconId);
    if (cit != cache.end()) return cit->second;

    // Not cached: the budget is per frame, and an icon that misses it shows
    // blank this frame and is asked for again next one. Caching a null here
    // would blacklist it for the life of the panel.
    if (!claimUiTextureUpload()) return VK_NULL_HANDLE;

    auto pit = paths.find(iconId);
    if (pit == paths.end()) {
        cache[iconId] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    // Cached either way, failures included: the file is either there or it is
    // not, and looking again every frame will not change that.
    VkDescriptorSet ds =
        uploadUiTextureFromBlp(assetManager, pit->second + ".blp", window);
    cache[iconId] = ds;
    return ds;
}

VkDescriptorSet itemIconTexture(uint32_t displayInfoId,
                                pipeline::AssetManager* assetManager,
                                core::Window* window) {
    if (displayInfoId == 0 || !assetManager) return VK_NULL_HANDLE;

    // Shared across the interface: the bags, the action bar, tooltips and the
    // dialogs all draw the same items.
    //
    // Two caches, because they bound different things. The first is a display
    // id to the icon it names; the second is an icon *path* to the texture
    // uploaded for it. Keying the upload by display id meant every id that
    // named an already-uploaded icon uploaded it again - and thousands of
    // items share a few hundred icon files, so a long session's GPU cost grew
    // with items seen rather than with icons seen. Now it grows with the
    // number of distinct icon files, which is a fixed and much smaller set.
    //
    // Neither evicts. Freeing a descriptor set that a recorded command buffer
    // still references is a use-after-free, so real eviction needs frame-fenced
    // destruction this file has no access to; bounding the upload set is the
    // part that can be done safely here.
    static std::unordered_map<uint32_t, std::string> pathOfDisplayId;
    static std::unordered_map<std::string, VkDescriptorSet> cache;
    if (auto pit = pathOfDisplayId.find(displayInfoId); pit != pathOfDisplayId.end()) {
        auto cit = cache.find(pit->second);
        if (cit != cache.end()) return cit->second;
    }

    // Deferred rather than cached as a miss: the budget refusing an upload
    // this frame says nothing about the icon.
    if (!claimUiTextureUpload()) return VK_NULL_HANDLE;

    auto dbc = assetManager->loadDBC("ItemDisplayInfo.dbc");
    if (!dbc) {
        core::Logger::getInstance().warning(
            "itemIconTexture: ItemDisplayInfo.dbc not loadable for displayInfoId=",
            displayInfoId);
        pathOfDisplayId[displayInfoId] = std::string();
        cache[std::string()] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    const int32_t recIdx = dbc->findRecordById(displayInfoId);
    if (recIdx < 0) {
        core::Logger::getInstance().warning(
            "itemIconTexture: displayInfoId=", displayInfoId,
            " not found in ItemDisplayInfo.dbc");
        pathOfDisplayId[displayInfoId] = std::string();
        cache[std::string()] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    const auto* layout = pipeline::getActiveDBCLayout()
                             ? pipeline::getActiveDBCLayout()->getLayout("ItemDisplayInfo")
                             : nullptr;
    const std::string iconName =
        dbc->getString(static_cast<uint32_t>(recIdx), layout ? (*layout)["InventoryIcon"] : 5);
    if (iconName.empty()) {
        core::Logger::getInstance().warning(
            "itemIconTexture: displayInfoId=", displayInfoId, " recIdx=", recIdx,
            " has empty iconName field");
        pathOfDisplayId[displayInfoId] = std::string();
        cache[std::string()] = VK_NULL_HANDLE;
        return VK_NULL_HANDLE;
    }

    const std::string iconPath = "Interface\\Icons\\" + iconName + ".blp";
    pathOfDisplayId[displayInfoId] = iconPath;
    // Another display id already uploaded this exact icon: nothing more to do.
    if (auto cit = cache.find(iconPath); cit != cache.end()) return cit->second;
    UiTextureLoad why{};
    VkDescriptorSet ds = uploadUiTextureFromBlp(assetManager, iconPath, window, &why);
    // Which of the two failures happened is worth saying: a missing file is a
    // gap in the assets, an undecodable one is a file we cannot read.
    if (why == UiTextureLoad::NotFound) {
        core::Logger::getInstance().warning(
            "itemIconTexture: BLP not found at '", iconPath,
            "' (displayInfoId=", displayInfoId, ")");
    } else if (why == UiTextureLoad::DecodeFailed) {
        core::Logger::getInstance().warning(
            "itemIconTexture: BLP decode failed for '", iconPath, "'");
    }
    cache[iconPath] = ds;
    return ds;
}

}  // namespace wowee::ui
