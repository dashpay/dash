#include "proposaltests.h"
#include <test/setup_common.h>

#include <qt/governancelist.h>

void ProposalTests::proposaltests()
{
    CGovernanceObject *pGovObj = new CGovernanceObject;
    Proposal proposal(pGovObj);
    proposal.m_currentDate = QDateTime(QDate(2022, 1, 2), QTime(12, 00));
    proposal.m_endDate = QDateTime(QDate(2022, 2, 9), QTime(12, 00));
    QVERIFY( proposal.paymentRemaining() == 1) ;

    proposal.m_currentDate = QDateTime(QDate(2022, 1, 2), QTime(12, 00));
    proposal.m_endDate = QDateTime(QDate(2022, 3, 5), QTime(12, 00));
    QVERIFY( proposal.paymentRemaining() == 2);

    proposal.m_currentDate = QDateTime(QDate(2022, 1, 2), QTime(12, 00));
    proposal.m_endDate = QDateTime(QDate(2022, 4, 10), QTime(12, 00));
    QVERIFY( proposal.paymentRemaining() == 3);
}
