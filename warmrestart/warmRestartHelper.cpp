#include <cassert>
#include <sstream>

#include "warmRestartHelper.h"


using namespace swss;


WarmStartHelper::WarmStartHelper(RedisPipeline      *pipeline,
                                 ProducerStateTable *syncTable,
                                 const std::string  &syncTableName,
                                 const std::string  &dockerName,
                                 const std::string  &appName) :
    m_restorationTable(pipeline, syncTableName, false),
    m_syncTable(syncTable),
    m_syncTableName(syncTableName),
    m_dockName(dockerName),
    m_appName(appName)
{
    WarmStart::initialize(appName, dockerName);
}


WarmStartHelper::~WarmStartHelper()
{
}


void WarmStartHelper::setState(WarmStart::WarmStartState state)
{
    WarmStart::setWarmStartState(m_appName, state);

    /* Caching warm-restart FSM state in local member */
    m_state = state;
}


WarmStart::WarmStartState WarmStartHelper::getState(void) const
{
    return m_state;
}


/*
 * To be called by each application to obtain the active/inactive state of
 * warm-restart functionality, and proceed to initialize the FSM accordingly.
 */
bool WarmStartHelper::isEnabled(void)
{
    bool enabled = WarmStart::checkWarmStart(m_appName, m_dockName);

    /*
     * If warm-restart feature is enabled for this application, proceed to
     * initialize its FSM, and clean any pending state that could be potentially
     * held in ProducerState queues.
     */
    if (enabled)
    {
        SWSS_LOG_NOTICE("Initializing Warm-Restart cycle for %s application.",
                        m_appName.c_str());

        setState(WarmStart::INITIALIZED);
        m_syncTable->clear();
    }

    /* Keeping track of warm-reboot active/inactive state */
    m_enabled = enabled;

    return enabled;
}


bool WarmStartHelper::isReconciled(void) const
{
    return (m_state == WarmStart::RECONCILED);
}


bool WarmStartHelper::inProgress(void) const
{
    return (m_enabled && m_state != WarmStart::RECONCILED);
}


uint32_t WarmStartHelper::getRestartTimer(void) const
{
    return WarmStart::getWarmStartTimer(m_appName, m_dockName);
}


/*
 * Invoked by warmStartHelper clients during initialization. All interested parties
 * are expected to call this method to upload their associated redisDB state into
 * a temporary buffer, which will eventually serve to resolve any conflict between
 * 'old' and 'new' state.
 */
bool WarmStartHelper::runRestoration()
{
    SWSS_LOG_NOTICE("Warm-Restart: Initiating AppDB restoration process for %s "
                    "application.", m_appName.c_str());

    m_restorationTable.getContent(m_restorationVector);

    /*
     * If there's no AppDB state to restore, then alert callee right away to avoid
     * iterating through the 'reconciliation' process.
     */
    if (!m_restorationVector.size())
    {
        SWSS_LOG_NOTICE("Warm-Restart: No records received from AppDB for %s "
                        "application.", m_appName.c_str());

        setState(WarmStart::RECONCILED);

        return false;
    }

    SWSS_LOG_NOTICE("Warm-Restart: Received %d records from AppDB for %s "
                    "application.",
                    static_cast<int>(m_restorationVector.size()),
                    m_appName.c_str());

    setState(WarmStart::RESTORED);

    SWSS_LOG_NOTICE("Warm-Restart: Completed AppDB restoration process for %s "
                    "application.", m_appName.c_str());

    return true;
}


void WarmStartHelper::insertRefreshMap(const KeyOpFieldsValuesTuple &kfv)
{
    const std::string key = kfvKey(kfv);

    m_refreshMap[key] = kfv;
}


/*
 * The reconciliation process takes place here. In essence, all we are doing
 * is comparing the restored elements (old state) with the refreshed/new ones
 * generated by the application once it completes its restart cycle. If a
 * state-diff is found between these two, we will be honoring the refreshed
 * one received from the application, and will proceed to push it down to AppDB.
 */
void WarmStartHelper::reconcile(void)
{
    SWSS_LOG_NOTICE("Warm-Restart: Initiating reconciliation process for %s "
                    "application.", m_appName.c_str());

    assert(getState() == WarmStart::RESTORED);

    for (auto &restoredElem : m_restorationVector)
    {
        std::string restoredKey  = kfvKey(restoredElem);
        auto restoredFV          = kfvFieldsValues(restoredElem);

        auto iter = m_refreshMap.find(restoredKey);

        /*
         * If the restored element is not found in the refreshMap, we must
         * push a delete operation for this entry.
         */
        if (iter == m_refreshMap.end())
        {
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: deleting stale entry %s",
                            printKFV(restoredKey, restoredFV).c_str());

            m_syncTable->del(restoredKey);
            continue;
        }

        /*
         * If an explicit delete request is sent by the application, process it
         * right away.
         */
        else if (kfvOp(iter->second) == DEL_COMMAND)
        {
            SWSS_LOG_NOTICE("Warm-Restart reconciliation: deleting entry %s",
                            printKFV(restoredKey, restoredFV).c_str());

            m_syncTable->del(restoredKey);
        }

        /*
         * If a matching entry is found in refreshMap, proceed to compare it
         * with its restored counterpart.
         */
        else
        {
            auto refreshedKey = kfvKey(iter->second);
            auto refreshedFV  = kfvFieldsValues(iter->second);

            if (compareAllFV(restoredFV, refreshedFV))
            {
                SWSS_LOG_NOTICE("Warm-Restart reconciliation: updating entry %s",
                                printKFV(refreshedKey, refreshedFV).c_str());

                m_syncTable->set(refreshedKey, refreshedFV);
            }
            else
            {
                SWSS_LOG_INFO("Warm-Restart reconciliation: no changes needed for "
                              "existing entry %s",
                              printKFV(refreshedKey, refreshedFV).c_str());
            }
        }

        /* Deleting the just-processed restored entry from the refreshMap */
        m_refreshMap.erase(restoredKey);
    }

    /*
     * Iterate through all the entries left in the refreshMap, which correspond
     * to brand-new entries to be pushed down to AppDB.
     */
    for (auto &kfv : m_refreshMap)
    {
        auto refreshedKey = kfvKey(kfv.second);
        auto refreshedFV  = kfvFieldsValues(kfv.second);

        SWSS_LOG_NOTICE("Warm-Restart reconciliation: introducing new entry %s",
                        printKFV(refreshedKey, refreshedFV).c_str());

        m_syncTable->set(refreshedKey, refreshedFV);
    }

    /* Clearing pending kfv's from refreshMap */
    m_refreshMap.clear();

    /* Clearing restoration vector */
    m_restorationVector.clear();

    setState(WarmStart::RECONCILED);

    SWSS_LOG_NOTICE("Warm-Restart: Concluded reconciliation process for %s "
                    "application.", m_appName.c_str());
}


/*
 * Compare all field-value-tuples within two vectors.
 *
 * Example: v1 {nexthop: 10.1.1.1, ifname: eth1}
 *          v2 {nexthop: 10.1.1.2, ifname: eth2}
 *
 * Returns:
 *
 *    'false' : If the content of both 'fields' and 'values' fully match
 *    'true'  : No full-match is found
 */
bool WarmStartHelper::compareAllFV(const std::vector<FieldValueTuple> &v1,
                                   const std::vector<FieldValueTuple> &v2)
{
    std::unordered_map<std::string, std::string> v1Map((v1.begin()), v1.end());

     /* Iterate though all v2 tuples to check if their content match v1 ones */
    for (auto &v2fv : v2)
    {
        auto v1Iter = v1Map.find(v2fv.first);
        /*
         * The sizes of both tuple-vectors should always match within any
         * given application. In other words, all fields within v1 should be
         * also present in v2.
         *
         * To make this possible, every application should continue relying on a
         * uniform schema to create/generate information. For example, fpmsyncd
         * will be always expected to push FieldValueTuples with "nexthop" and
         * "ifname" fields; neighsyncd is expected to make use of "family" and
         * "neigh" fields, etc. The existing reconciliation logic will rely on
         * this assumption.
         */
        assert(v1Iter != v1Map.end());

        if (compareOneFV(v1Map[fvField(*v1Iter)], fvValue(v2fv)))
        {
            return true;
        }
    }

    return false;
}


/*
 * Compare the values of a single field-value within two different KFVs.
 *
 * Example: s1 {nexthop: 10.1.1.1, 10.1.1.2}
 *          s2 {nexthop: 10.1.1.2, 10.1.1.1}
 *
 * Example: s1 {Ethernet1, Ethernet2}
 *          s2 {Ethernet2, Ethernet1}
 *
 * Returns:
 *
 *    'false' : If the content of both strings fully matches
 *    'true'  : No full-match is found
 */
bool WarmStartHelper::compareOneFV(const std::string &s1, const std::string &s2)
{
    if (s1.size() != s2.size())
    {
        return true;
    }

    std::vector<std::string> splitValuesS1 = tokenize(s1, ',');
    std::vector<std::string> splitValuesS2 = tokenize(s2, ',');

    if (splitValuesS1.size() != splitValuesS2.size())
    {
        return true;
    }

    std::sort(splitValuesS1.begin(), splitValuesS1.end());
    std::sort(splitValuesS2.begin(), splitValuesS2.end());

    for (size_t i = 0; i < splitValuesS1.size(); i++)
    {
        if (splitValuesS1[i] != splitValuesS2[i])
        {
            return true;
        }
    }

    return false;
}


/*
 * Helper method to print KFVs in a friendly fashion.
 *
 * Example:
 *
 * 192.168.1.0/30 { nexthop: 10.2.2.1,10.1.2.1 | ifname: Ethernet116,Ethernet112 }
 */
const std::string WarmStartHelper::printKFV(const std::string                  &key,
                                            const std::vector<FieldValueTuple> &fv)
{
    std::string res;

    res = key + " { ";

    for (size_t i = 0; i < fv.size(); ++i)
    {
        res += fv[i].first + ": " +  fv[i].second;

        if (i != fv.size() - 1)
        {
            res += " | ";
        }
    }

    res += " } ";

    return res;
}
