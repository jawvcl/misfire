#include <Geode/Geode.hpp>
#include <Geode/modify/MenuLayer.hpp>
#include <Geode/modify/LevelSearchLayer.hpp>
#include <Geode/loader/Loader.hpp>
#include <Geode/binding/AppDelegate.hpp>
#include <Geode/binding/GJListLayer.hpp>
#include <Geode/binding/CustomListView.hpp>
#include <Geode/binding/GameLevelManager.hpp>
#include <Geode/binding/GJSearchObject.hpp>
#include <Geode/binding/LevelManagerDelegate.hpp>
#include <Geode/binding/LevelDownloadDelegate.hpp>
#include <Geode/binding/GJGameLevel.hpp>
#include <Geode/binding/LoadingCircle.hpp>
#include <Geode/ui/TextInput.hpp>
#include <Geode/ui/BasedButtonSprite.hpp>
#include <Geode/utils/string.hpp>
#include <matjson.hpp>
#include <thread>
#include <unordered_set>
#include <windows.h>
#include <winhttp.h>
#include <unordered_map>
#include <algorithm>

using namespace geode::prelude;

// ---------------- 데이터 (Supabase 챌린지 메타데이터) ----------------
struct ChallengeItem {
    std::string type;
    std::string name;
    std::string creator;
    int levelId = 0;
    int position = 0;
};

static std::vector<ChallengeItem> g_classicItems;
static std::vector<ChallengeItem> g_platformerItems;
static bool g_showPlatformer = false;

// ---------------- 네트워킹 (Supabase) ----------------
static const wchar_t* SUPABASE_HOST = L"erpdomfrjxblrmapoceb.supabase.co";
static const wchar_t* SUPABASE_APIKEY = L"sb_publishable_v44QyS3zcgDfpSSavZ3CAw_JuKL2Jfi";

static std::string httpsGet(std::wstring const& host, std::wstring const& path, std::wstring const& apiKey) {
    std::string result;

    HINTERNET hSession = WinHttpOpen(
        L"JawvCL/1.0",
        WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!hSession) return result;

    HINTERNET hConnect = WinHttpConnect(hSession, host.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return result; }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        NULL, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!hRequest) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); return result; }

    std::wstring headers = L"apikey: " + apiKey + L"\r\n";

    BOOL sent = WinHttpSendRequest(
        hRequest, headers.c_str(), (DWORD)-1L,
        WINHTTP_NO_REQUEST_DATA, 0, 0, 0
    );

    if (sent && WinHttpReceiveResponse(hRequest, NULL)) {
        DWORD size = 0;
        do {
            DWORD downloaded = 0;
            if (!WinHttpQueryDataAvailable(hRequest, &size)) break;
            if (size == 0) break;

            std::vector<char> buffer(size + 1, 0);
            if (WinHttpReadData(hRequest, buffer.data(), size, &downloaded)) {
                result.append(buffer.data(), downloaded);
            }
        } while (size > 0);
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    return result;
}

static void parseChallengeList(std::string const& json) {
    auto parsed = matjson::parse(json);
    if (parsed.isErr()) return;

    auto arr = parsed.unwrap();
    if (!arr.isArray()) return;

    auto arrResult = arr.asArray();
    if (arrResult.isErr()) return;

    std::vector<ChallengeItem> classicItems;
    std::vector<ChallengeItem> platformerItems;

    for (auto const& entry : arrResult.unwrap()) {
        ChallengeItem item;
        item.type = entry["type"].asString().unwrapOr("");
        item.name = entry["name"].asString().unwrapOr("");
        item.creator = entry["creator"].asString().unwrapOr("");
        item.position = entry["position"].asInt().unwrapOr(0);

        auto levelIdStr = entry["level_id"].asString().unwrapOr("0");
        item.levelId = std::atoi(levelIdStr.c_str());

        if (item.type == "classic") {
            classicItems.push_back(item);
        } else if (item.type == "platformer") {
            platformerItems.push_back(item);
        }
    }

    auto sortByPosition = [](ChallengeItem const& a, ChallengeItem const& b) {
        return a.position < b.position;
    };
    std::sort(classicItems.begin(), classicItems.end(), sortByPosition);
    std::sort(platformerItems.begin(), platformerItems.end(), sortByPosition);

    g_classicItems = std::move(classicItems);
    g_platformerItems = std::move(platformerItems);
}

static void fetchChallengeList() {
    std::thread([]() {
        std::wstring path = L"/rest/v1/maps?select=*&order=type.asc,position.asc&limit=100";
        std::string json = httpsGet(SUPABASE_HOST, path, SUPABASE_APIKEY);

        Loader::get()->queueInMainThread([json]() {
            parseChallengeList(json);
        });
    }).detach();
}

// ---------------- 전체 화면 챌린지 리스트 레이어 ----------------
class ChallengeListLayer : public CCLayer, public LevelManagerDelegate, public LevelDownloadDelegate {
protected:
    GJListLayer* m_list = nullptr;
    LoadingCircle* m_loadingCircle = nullptr;
    CCLabelBMFont* m_countLabel = nullptr;
    CCLabelBMFont* m_pageLabel = nullptr;
    CCMenuItemSpriteExtra* m_leftButton = nullptr;
    CCMenuItemSpriteExtra* m_rightButton = nullptr;
    CCMenuItemSpriteExtra* m_firstButton = nullptr;
    CCMenuItemSpriteExtra* m_lastButton = nullptr;
    CCMenuItemSpriteExtra* m_starToggle = nullptr;
    CCMenuItemSpriteExtra* m_moonToggle = nullptr;
    CCMenu* m_searchBarMenu = nullptr;
    TextInput* m_searchBar = nullptr;

    std::vector<ChallengeItem> m_fullSearchResults;
    std::string m_query;
    int m_page = 0;

    // 언리스트 레벨 개별 다운로드 폴백용
    CCArray* m_collectedLevels = nullptr;
    std::vector<int> m_fallbackPendingIds;
    bool m_fallbackActive = false;

    bool init() {
        if (!CCLayer::init()) return false;

        auto winSize = CCDirector::sharedDirector()->getWinSize();

        auto bg = CCSprite::create("GJ_gradientBG.png");
        bg->setAnchorPoint({ 0.f, 0.f });
        bg->setScaleX((winSize.width + 10.f) / bg->getTextureRect().size.width);
        bg->setScaleY((winSize.height + 10.f) / bg->getTextureRect().size.height);
        bg->setPosition({ -5.f, -5.f });
        bg->setColor({ 51, 51, 51 });
        addChild(bg);

        auto bottomLeftCorner = CCSprite::createWithSpriteFrameName("gauntletCorner_001.png");
        bottomLeftCorner->setPosition({ -1.f, -1.f });
        bottomLeftCorner->setAnchorPoint({ 0.f, 0.f });
        addChild(bottomLeftCorner);

        auto bottomRightCorner = CCSprite::createWithSpriteFrameName("gauntletCorner_001.png");
        bottomRightCorner->setPosition({ winSize.width + 1.f, -1.f });
        bottomRightCorner->setAnchorPoint({ 1.f, 0.f });
        bottomRightCorner->setFlipX(true);
        addChild(bottomRightCorner);

        m_countLabel = CCLabelBMFont::create("", "goldFont.fnt");
        m_countLabel->setAnchorPoint({ 1.f, 1.f });
        m_countLabel->setScale(0.6f);
        m_countLabel->setPosition({ winSize.width - 7.f, winSize.height - 3.f });
        addChild(m_countLabel);

        m_list = GJListLayer::create(nullptr, "JawV Challenge List", { 0, 0, 0, 180 }, 356.f, 220.f, 0);
        m_list->setPosition(winSize / 2.f - m_list->getContentSize() / 2.f);
        addChild(m_list, 2);

        m_searchBarMenu = CCMenu::create();
        m_searchBarMenu->setContentSize({ 356.f, 30.f });
        m_searchBarMenu->setPosition({ 0.f, 190.f });
        m_list->addChild(m_searchBarMenu);

        auto searchBackground = CCLayerColor::create({ 194, 114, 62, 255 }, 356.f, 30.f);
        m_searchBarMenu->addChild(searchBackground);

        auto searchSprite = CCSprite::createWithSpriteFrameName("gj_findBtn_001.png");
        searchSprite->setScale(0.7f);
        auto searchButton = CCMenuItemSpriteExtra::create(searchSprite, this, menu_selector(ChallengeListLayer::onSearch));
        searchButton->setPosition({ 337.f, 15.f });
        m_searchBarMenu->addChild(searchButton);

        m_searchBar = TextInput::create(310.f, "Search Levels...");
        m_searchBar->setPosition({ 165.f, 15.f });
        m_searchBar->setTextAlign(TextInputAlign::Left);
        m_searchBarMenu->addChild(m_searchBar);

        auto menu = CCMenu::create();
        menu->setPosition({ 0.f, 0.f });
        addChild(menu);

        auto backButton = CCMenuItemSpriteExtra::create(
            CCSprite::createWithSpriteFrameName("GJ_arrow_01_001.png"), this, menu_selector(ChallengeListLayer::onBack)
        );
        backButton->setPosition({ 25.f, winSize.height - 25.f });
        menu->addChild(backButton);

        auto leftBtnSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
        m_leftButton = CCMenuItemSpriteExtra::create(leftBtnSpr, this, menu_selector(ChallengeListLayer::onPrevPage));
        m_leftButton->setPosition({ 24.f, winSize.height / 2.f });
        menu->addChild(m_leftButton);

        auto rightBtnSpr = CCSprite::createWithSpriteFrameName("GJ_arrow_03_001.png");
        rightBtnSpr->setFlipX(true);
        m_rightButton = CCMenuItemSpriteExtra::create(rightBtnSpr, this, menu_selector(ChallengeListLayer::onNextPage));
        m_rightButton->setPosition({ winSize.width - 24.f, winSize.height / 2.f });
        menu->addChild(m_rightButton);

        // 새로고침 버튼 - 화면 오른쪽 아래
        auto refreshBtnSpr = CCSprite::createWithSpriteFrameName("GJ_updateBtn_001.png");
        auto refreshButton = CCMenuItemSpriteExtra::create(refreshBtnSpr, this, menu_selector(ChallengeListLayer::onRefresh));
        refreshButton->setPosition({ winSize.width - refreshBtnSpr->getContentWidth() / 2.f - 4.f, refreshBtnSpr->getContentHeight() / 2.f + 4.f });
        menu->addChild(refreshButton, 2);

        // classic/platformer 토글 (별/달 아이콘)
        auto starSprite = CCSprite::createWithSpriteFrameName("GJ_starsIcon_001.png");
        starSprite->setScale(1.1f);
        m_starToggle = CCMenuItemSpriteExtra::create(starSprite, this, menu_selector(ChallengeListLayer::onStar));
        m_starToggle->setPosition({ 30.f, 60.f });
        m_starToggle->setColor(g_showPlatformer ? ccColor3B{ 125, 125, 125 } : ccColor3B{ 255, 255, 255 });
        menu->addChild(m_starToggle, 2);

        auto moonSprite = CCSprite::createWithSpriteFrameName("GJ_moonsIcon_001.png");
        moonSprite->setScale(1.1f);
        m_moonToggle = CCMenuItemSpriteExtra::create(moonSprite, this, menu_selector(ChallengeListLayer::onMoon));
        m_moonToggle->setPosition({ 60.f, 60.f });
        m_moonToggle->setColor(g_showPlatformer ? ccColor3B{ 255, 255, 255 } : ccColor3B{ 125, 125, 125 });
        menu->addChild(m_moonToggle, 2);

        // 페이지 번호 표시 (오른쪽 위, 클릭 비활성 - 숫자만 보여줌)
        m_pageLabel = CCLabelBMFont::create("1", "bigFont.fnt");
        m_pageLabel->setScale(0.8f);
        auto pageBg = CCSprite::create("GJ_button_02.png");
        pageBg->setScale(0.7f);
        m_pageLabel->setPosition(pageBg->getContentSize() / 2.f);
        pageBg->addChild(m_pageLabel);
        auto pageDummyMenuItem = CCMenuItemSpriteExtra::create(pageBg, this, nullptr);
        pageDummyMenuItem->setPositionY(winSize.height - 39.5f);
        pageDummyMenuItem->setPositionX(winSize.width - pageBg->getContentWidth() * 0.35f - 3.f);
        pageDummyMenuItem->setEnabled(false);
        menu->addChild(pageDummyMenuItem);

        // 끝 페이지로 이동 (페이지 번호 바로 아래)
        auto lastArrow = CCSprite::createWithSpriteFrameName("GJ_arrow_02_001.png");
        lastArrow->setFlipX(true);
        lastArrow->setScale(0.5f);
        m_lastButton = CCMenuItemSpriteExtra::create(lastArrow, this, menu_selector(ChallengeListLayer::onLast));
        m_lastButton->setPositionY(pageDummyMenuItem->getPositionY() - pageBg->getContentHeight() / 2.f - m_lastButton->getContentHeight() / 2.f - 5.f);
        m_lastButton->setPositionX(pageDummyMenuItem->getPositionX());
        menu->addChild(m_lastButton);

        // 처음 페이지로 이동 (왼쪽, 끝 버튼과 같은 높이)
        auto firstArrow = CCSprite::createWithSpriteFrameName("GJ_arrow_02_001.png");
        firstArrow->setScale(0.5f);
        m_firstButton = CCMenuItemSpriteExtra::create(firstArrow, this, menu_selector(ChallengeListLayer::onFirst));
        m_firstButton->setPosition({ 21.5f, m_lastButton->getPositionY() });
        menu->addChild(m_firstButton);

        m_loadingCircle = LoadingCircle::create();
        m_loadingCircle->setParentLayer(this);
        m_loadingCircle->show();

        showLoading();

        populateList("");

        return true;
    }

    void onExit() {
        auto glm = GameLevelManager::sharedState();
        if (glm->m_levelManagerDelegate == this) {
            glm->m_levelManagerDelegate = nullptr;
        }
        if (glm->m_levelDownloadDelegate == this) {
            glm->m_levelDownloadDelegate = nullptr;
        }
        CCLayer::onExit();
    }

    void onBack(CCObject*) {
        CCDirector::sharedDirector()->popSceneWithTransition(0.5f, kPopTransitionFade);
    }

    void onPrevPage(CCObject*) { page(m_page - 1); }
    void onNextPage(CCObject*) { page(m_page + 1); }
    void onFirst(CCObject*) { page(0); }
    void onLast(CCObject*) { page((int)((m_fullSearchResults.size() - 1) / 10)); }

    void onRefresh(CCObject*) {
        showLoading();
        fetchChallengeList();
        // 데이터가 새로 들어오는 건 비동기라, 잠깐 뒤 다시 목록 구성
        this->runAction(CCSequence::create(
            CCDelayTime::create(1.0f),
            CCCallFunc::create(this, callfunc_selector(ChallengeListLayer::onRefreshFinished)),
            nullptr
        ));
    }

    void onRefreshFinished() {
        populateList(m_query);
    }

    void onStar(CCObject*) {
        if (!g_showPlatformer) return;
        g_showPlatformer = false;
        m_starToggle->setColor({ 255, 255, 255 });
        m_moonToggle->setColor({ 125, 125, 125 });
        if (auto listTitle = static_cast<CCLabelBMFont*>(m_list->getChildByID("title"))) {
            listTitle->setString("JawV Challenge List");
        }
        showLoading();
        m_page = 0;
        populateList(m_query);
    }

    void onMoon(CCObject*) {
        if (g_showPlatformer) return;
        g_showPlatformer = true;
        m_starToggle->setColor({ 125, 125, 125 });
        m_moonToggle->setColor({ 255, 255, 255 });
        if (auto listTitle = static_cast<CCLabelBMFont*>(m_list->getChildByID("title"))) {
            listTitle->setString("JawV Challenge List");
        }
        showLoading();
        m_page = 0;
        populateList(m_query);
    }

    void onSearch(CCObject*) {
        auto query = m_searchBar->getString();
        if (m_query != query) {
            m_page = 0;
            showLoading();
            populateList(query);
        }
    }

    void showLoading() {
        m_pageLabel->setString(fmt::to_string(m_page + 1).c_str());
        m_loadingCircle->setVisible(true);
        if (auto listView = m_list->m_listView) listView->setVisible(false);
        m_searchBarMenu->setVisible(false);
        m_countLabel->setVisible(false);
        m_leftButton->setVisible(false);
        m_rightButton->setVisible(false);
        m_firstButton->setVisible(false);
        m_lastButton->setVisible(false);
    }

    std::vector<int> currentPageIds() {
        std::vector<int> ids;
        int start = m_page * 10;
        int end = std::min((int)m_fullSearchResults.size(), (m_page + 1) * 10);
        for (int i = start; i < end; i++) {
            ids.push_back(m_fullSearchResults[i].levelId);
        }
        return ids;
    }

    void populateList(std::string const& query) {
        m_fullSearchResults.clear();

        auto& source = g_showPlatformer ? g_platformerItems : g_classicItems;

        if (query.empty()) {
            m_fullSearchResults = source;
        } else {
            auto lowerQuery = string::toLower(query);
            for (auto const& item : source) {
                if (string::toLower(item.name).find(lowerQuery) != std::string::npos) {
                    m_fullSearchResults.push_back(item);
                }
            }
        }

        m_query = query;

        if (m_fullSearchResults.empty()) {
            finalizeList(CCArray::create());
            m_countLabel->setString("");
            return;
        }

        auto pageIds = currentPageIds();

        std::string idList;
        for (size_t i = 0; i < pageIds.size(); i++) {
            if (i > 0) idList += ",";
            idList += std::to_string(pageIds[i]);
        }

        GameLevelManager::sharedState()->m_levelManagerDelegate = this;

        auto searchObj = GJSearchObject::create(SearchType::Type19);
        searchObj->m_searchQuery = idList;
        GameLevelManager::sharedState()->getOnlineLevels(searchObj);
    }

    // ---------- LevelManagerDelegate ----------
    void loadLevelsFinished(cocos2d::CCArray* levels, char const* key) {
        auto pageIds = currentPageIds();

        std::unordered_set<int> foundIds;
        auto collected = CCArray::create();
        if (levels) {
            for (unsigned int i = 0; i < levels->count(); i++) {
                auto lvl = static_cast<GJGameLevel*>(levels->objectAtIndex(i));
                if (lvl) {
                    foundIds.insert(lvl->m_levelID.value());
                    collected->addObject(lvl);
                }
            }
        }

        std::vector<int> missing;
        for (int id : pageIds) {
            if (!foundIds.count(id)) missing.push_back(id);
        }

        if (missing.empty()) {
            finalizeList(collected);
            return;
        }

        // 일부(주로 언리스트 레벨)가 검색에서 빠졌음 - 개별 다운로드로 재시도
        startFallbackDownload(collected, missing);
    }

    void loadLevelsFailed(char const* key) {
        // 검색 요청 자체가 통째로 실패 (보통 언리스트 레벨이 섞여있을 때) - 페이지 전체를 개별 다운로드로 재시도
        auto pageIds = currentPageIds();
        startFallbackDownload(CCArray::create(), pageIds);
    }

    // ---------- 언리스트 레벨 개별 다운로드 폴백 ----------
    void startFallbackDownload(CCArray* alreadyCollected, std::vector<int> const& idsToFetch) {
        if (m_collectedLevels) {
            m_collectedLevels->release();
        }
        m_collectedLevels = alreadyCollected;
        m_collectedLevels->retain();

        m_fallbackPendingIds = idsToFetch;
        m_fallbackActive = true;

        GameLevelManager::sharedState()->m_levelDownloadDelegate = this;

        for (int id : idsToFetch) {
            GameLevelManager::sharedState()->downloadLevel(id, false, -1);
        }
    }

    // ---------- LevelDownloadDelegate ----------
    void levelDownloadFinished(GJGameLevel* level) {
        if (!m_fallbackActive) return;

        if (level) {
            m_collectedLevels->addObject(level);
            auto it = std::find(m_fallbackPendingIds.begin(), m_fallbackPendingIds.end(), level->m_levelID.value());
            if (it != m_fallbackPendingIds.end()) m_fallbackPendingIds.erase(it);
        }

        if (m_fallbackPendingIds.empty()) {
            m_fallbackActive = false;
            finalizeList(m_collectedLevels);
        }
    }

    void levelDownloadFailed(int response) {
        if (!m_fallbackActive) return;

        // 어떤 ID가 실패했는지는 알 수 없으니, 하나 제거하고 계속 진행
        if (!m_fallbackPendingIds.empty()) {
            m_fallbackPendingIds.erase(m_fallbackPendingIds.begin());
        }

        if (m_fallbackPendingIds.empty()) {
            m_fallbackActive = false;
            finalizeList(m_collectedLevels);
        }
    }

    // ---------- 최종 리스트 화면 구성 ----------
    void finalizeList(cocos2d::CCArray* rawLevels) {
        // 우리가 알고 있는 순위(position) 순서대로 재정렬
        // (배치 검색 결과 순서가 보장 안 되고, 언리스트 레벨 폴백은 항상 맨 끝에 붙기 때문)
        auto pageIds = currentPageIds();
        std::unordered_map<int, int> orderIndex;
        for (size_t i = 0; i < pageIds.size(); i++) {
            orderIndex[pageIds[i]] = (int)i;
        }

        std::vector<GJGameLevel*> sortedVec;
        if (rawLevels) {
            for (unsigned int i = 0; i < rawLevels->count(); i++) {
                if (auto lvl = static_cast<GJGameLevel*>(rawLevels->objectAtIndex(i))) {
                    sortedVec.push_back(lvl);
                }
            }
        }
        std::sort(sortedVec.begin(), sortedVec.end(), [&](GJGameLevel* a, GJGameLevel* b) {
            int ia = orderIndex.count(a->m_levelID.value()) ? orderIndex[a->m_levelID.value()] : INT_MAX;
            int ib = orderIndex.count(b->m_levelID.value()) ? orderIndex[b->m_levelID.value()] : INT_MAX;
            return ia < ib;
        });

        auto levels = CCArray::create();
        for (auto lvl : sortedVec) levels->addObject(lvl);

        if (auto listView = m_list->m_listView) {
            listView->removeFromParent();
            listView->release();
        }

        auto listView = CustomListView::create(levels, BoomListType::Level, 190.f, 356.f);
        listView->retain();
        m_list->addChild(listView, 6, 9);
        m_list->m_listView = listView;

        m_searchBarMenu->setVisible(true);
        m_countLabel->setVisible(true);
        m_loadingCircle->setVisible(false);

        auto size = m_fullSearchResults.size();
        m_countLabel->setString(fmt::format("{} to {} of {}",
            size == 0 ? 0 : m_page * 10 + 1,
            std::min<int>((int)size, (m_page + 1) * 10), size).c_str());
        m_countLabel->limitLabelWidth(100.f, 0.6f, 0.f);

        if (size > 10) {
            auto maxPage = (int)((size - 1) / 10);
            m_leftButton->setVisible(m_page > 0);
            m_rightButton->setVisible(m_page < maxPage);
            m_firstButton->setVisible(m_page > 0);
            m_lastButton->setVisible(m_page < maxPage);
        }
    }

    void page(int newPage) {
        if (m_fullSearchResults.empty()) return;
        auto maxPage = (int)((m_fullSearchResults.size() + 9) / 10);
        m_page = maxPage > 0 ? ((newPage % maxPage) + maxPage) % maxPage : 0;
        showLoading();
        populateList(m_query);
    }

public:
    static ChallengeListLayer* create() {
        auto ret = new ChallengeListLayer();
        if (ret->init()) {
            ret->autorelease();
            return ret;
        }
        delete ret;
        return nullptr;
    }

    static CCScene* scene() {
        auto ret = CCScene::create();
        ret->addChild(ChallengeListLayer::create());
        return ret;
    }
};

// ---------------- 트리거 버튼 ----------------
class $modify(MyLevelSearchLayer, LevelSearchLayer) {
    bool init(int type) {
        if (!LevelSearchLayer::init(type)) return false;

        if (auto sideMenu = this->getChildByID("other-filter-menu")) {
            auto btnSpr = geode::CircleButtonSprite::createWithSprite(
                "horn.png"_spr, 0.9f,
                geode::CircleBaseColor::Green,
                geode::CircleBaseSize::Small
            );

            auto btn = CCMenuItemSpriteExtra::create(
                btnSpr, this, menu_selector(MyLevelSearchLayer::onOpenChallengeList)
            );
            btn->setID("jawvcl-list-button");
            sideMenu->addChild(btn);
            static_cast<CCMenu*>(sideMenu)->updateLayout();
        }

        return true;
    }

    void onOpenChallengeList(CCObject*) {
        CCDirector::sharedDirector()->pushScene(CCTransitionFade::create(0.5f, ChallengeListLayer::scene()));
    }
};

// ---------------- 데이터 로드 트리거 ----------------
class $modify(MyMenuLayer, MenuLayer) {
    bool init() {
        if (!MenuLayer::init()) {
            return false;
        }

        fetchChallengeList();

        return true;
    }
};