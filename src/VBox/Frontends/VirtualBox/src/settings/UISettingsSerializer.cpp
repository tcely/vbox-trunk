﻿/* $Id$ */
/** @file
 * VBox Qt GUI - UISettingsSerializer class implementation.
 */

/*
 * Copyright (C) 2006-2015 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifdef VBOX_WITH_PRECOMPILED_HEADERS
# include <precomp.h>
#else  /* !VBOX_WITH_PRECOMPILED_HEADERS */
/* Qt includes: */
# include <QTimer>
# include <QLabel>
# include <QHBoxLayout>
# include <QVBoxLayout>
# include <QProgressBar>
/* GUI includes: */
# include "UISettingsSerializer.h"
# include "UISettingsPage.h"
# include "UIIconPool.h"
#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */

UISettingsSerializer::UISettingsSerializer(QObject *pParent, SerializationDirection direction,
                                           const QVariant &data, const UISettingsPageList &pages)
    : QThread(pParent)
    , m_direction(direction)
    , m_data(data)
    , m_fSavingComplete(m_direction == Load)
    , m_iIdOfHighPriorityPage(-1)
{
    /* Copy the page(s) from incoming list to our map: */
    foreach (UISettingsPage *pPage, pages)
        m_pages.insert(pPage->id(), pPage);

    /* Handling internal signals, they are also redirected in their handlers: */
    connect(this, SIGNAL(sigNotifyAboutPageProcessed(int)), this, SLOT(sltHandleProcessedPage(int)), Qt::QueuedConnection);
    connect(this, SIGNAL(sigNotifyAboutPagesProcessed()), this, SLOT(sltHandleProcessedPages()), Qt::QueuedConnection);

    /* Redirecting unhandled internal signals: */
    connect(this, SIGNAL(finished()), this, SIGNAL(sigNotifyAboutProcessFinished()), Qt::QueuedConnection);
}

UISettingsSerializer::~UISettingsSerializer()
{
    /* If serializer is being destructed by it's parent,
     * thread could still be running, we have to wait
     * for it to be finished! */
    if (isRunning())
        wait();
}

void UISettingsSerializer::raisePriorityOfPage(int iPageId)
{
    /* If that page is present and was not processed already =>
     * we should remember which page should be processed next: */
    if (m_pages.contains(iPageId) && !(m_pages[iPageId]->processed()))
        m_iIdOfHighPriorityPage = iPageId;
}

void UISettingsSerializer::start(Priority priority /* = InheritPriority */)
{
    /* Notify listeners about we are starting: */
    emit sigNotifyAboutProcessStarted();

    /* If serializer saves settings: */
    if (m_direction == Save)
    {
        /* We should update internal page cache first: */
        foreach (UISettingsPage *pPage, m_pages.values())
            pPage->putToCache();
    }

    /* Start async serializing thread: */
    QThread::start(priority);
}

void UISettingsSerializer::sltHandleProcessedPage(int iPageId)
{
    /* If serializer loads settings: */
    if (m_direction == Load)
    {
        /* If such page present: */
        if (m_pages.contains(iPageId))
        {
            /* We should fetch internal page cache: */
            UISettingsPage *pSettingsPage = m_pages[iPageId];
            pSettingsPage->setValidatorBlocked(true);
            pSettingsPage->getFromCache();
            pSettingsPage->setValidatorBlocked(false);
        }
    }
    /* Notify listeners about page postprocessed: */
    emit sigNotifyAboutPagePostprocessed(iPageId);
}

void UISettingsSerializer::sltHandleProcessedPages()
{
    /* If serializer saves settings: */
    if (m_direction == Save)
    {
        /* We should flag GUI thread to unlock itself: */
        if (!m_fSavingComplete)
            m_fSavingComplete = true;
    }
    /* If serializer loads settings: */
    else
    {
        /* We have to do initial validation finally: */
        foreach (UISettingsPage *pPage, m_pages.values())
            pPage->revalidate();
    }
    /* Notify listeners about pages postprocessed: */
    emit sigNotifyAboutPagesPostprocessed();
}

void UISettingsSerializer::run()
{
    /* Initialize COM for other thread: */
    COMBase::InitializeCOM(false);

    /* Mark all the pages initially as NOT processed: */
    foreach (UISettingsPage *pPage, m_pages.values())
        pPage->setProcessed(false);

    /* Iterate over the all left settings pages: */
    UISettingsPageMap pages(m_pages);
    while (!pages.empty())
    {
        /* Get required page pointer, protect map by mutex while getting pointer: */
        UISettingsPage *pPage = m_iIdOfHighPriorityPage != -1 && pages.contains(m_iIdOfHighPriorityPage) ?
                                pages.value(m_iIdOfHighPriorityPage) : *pages.begin();
        /* Reset request of high priority: */
        if (m_iIdOfHighPriorityPage != -1)
            m_iIdOfHighPriorityPage = -1;
        /* Process this page if its enabled: */
        if (pPage->isEnabled())
        {
            if (m_direction == Load)
                pPage->loadToCacheFrom(m_data);
            if (m_direction == Save)
                pPage->saveFromCacheTo(m_data);
        }
        /* Remember what page was processed: */
        pPage->setProcessed(true);
        /* Remove processed page from our map: */
        pages.remove(pPage->id());
        /* Notify listeners about page was processed: */
        emit sigNotifyAboutPageProcessed(pPage->id());
        /* If serializer saves settings => wake up GUI thread: */
        if (m_direction == Save)
            m_condition.wakeAll();
        /* Break further processing if page had failed: */
        if (pPage->failed())
            break;
    }
    /* Notify listeners about all pages were processed: */
    emit sigNotifyAboutPagesProcessed();
    /* If serializer saves settings => wake up GUI thread: */
    if (m_direction == Save)
        m_condition.wakeAll();

    /* Deinitialize COM for other thread: */
    COMBase::CleanupCOM();
}

UISettingsSerializerProgress::UISettingsSerializerProgress(QWidget *pParent, UISettingsSerializer::SerializationDirection direction,
                                                           const QVariant &data, const UISettingsPageList &pages)
    : QIWithRetranslateUI<QIDialog>(pParent)
    , m_direction(direction)
    , m_data(data)
    , m_pages(pages)
    , m_pSerializer(0)
    , m_pLabelOperationProgress(0)
    , m_pBarOperationProgress(0)
{
    /* Prepare: */
    prepare();
    /* Translate: */
    retranslateUi();
}

int UISettingsSerializerProgress::exec()
{
    /* Ask for process start: */
    emit sigAskForProcessStart();

    /* Call to base-class: */
    return QIWithRetranslateUI<QIDialog>::exec();
}

QVariant& UISettingsSerializerProgress::data()
{
    AssertPtrReturn(m_pSerializer, m_data);
    return m_pSerializer->data();
}

void UISettingsSerializerProgress::prepare()
{
    /* Configure self: */
    setWindowModality(Qt::WindowModal);
    setWindowTitle(parentWidget()->windowTitle());
    connect(this, SIGNAL(sigAskForProcessStart()),
            this, SLOT(sltStartProcess()), Qt::QueuedConnection);

    /* Create serializer: */
    m_pSerializer = new UISettingsSerializer(this, m_direction, m_data, m_pages);
    AssertPtrReturnVoid(m_pSerializer);
    {
        /* Install progress handler: */
        connect(m_pSerializer, SIGNAL(sigNotifyAboutPagePostprocessed(int)),
                this, SLOT(sltAdvanceProgressValue()));
        connect(m_pSerializer, SIGNAL(sigNotifyAboutPagesPostprocessed()),
                this, SLOT(sltAdvanceProgressValue()));
    }

    /* Create layout: */
    QVBoxLayout *pLayout = new QVBoxLayout(this);
    AssertPtrReturnVoid(pLayout);
    {
        /* Create top layout: */
        QHBoxLayout *pLayoutTop = new QHBoxLayout;
        AssertPtrReturnVoid(pLayoutTop);
        {
            /* Create pixmap layout: */
            QVBoxLayout *pLayoutPixmap = new QVBoxLayout;
            AssertPtrReturnVoid(pLayoutPixmap);
            {
                /* Create pixmap label: */
                QLabel *pLabelPixmap = new QLabel;
                AssertPtrReturnVoid(pLabelPixmap);
                {
                    /* Configure label: */
                    const QIcon icon = UIIconPool::iconSet(":/progress_settings_90px.png");
                    pLabelPixmap->setPixmap(icon.pixmap(icon.availableSizes().first()));
                    /* Add label into layout: */
                    pLayoutPixmap->addWidget(pLabelPixmap);
                }
                /* Add stretch: */
                pLayoutPixmap->addStretch();
                /* Add layout into parent: */
                pLayoutTop->addLayout(pLayoutPixmap);
            }
            /* Create progress layout: */
            QVBoxLayout *pLayoutProgress = new QVBoxLayout;
            AssertPtrReturnVoid(pLayoutProgress);
            {
                /* Create operation progress label: */
                m_pLabelOperationProgress = new QLabel;
                AssertPtrReturnVoid(m_pLabelOperationProgress);
                {
                    /* Add label into layout: */
                    pLayoutProgress->addWidget(m_pLabelOperationProgress);
                }
                /* Create operation progress bar: */
                m_pBarOperationProgress = new QProgressBar;
                AssertPtrReturnVoid(m_pBarOperationProgress);
                {
                    /* Configure progress bar: */
                    m_pBarOperationProgress->setMinimumWidth(300);
                    m_pBarOperationProgress->setMaximum(m_pSerializer->pageCount() + 1);
                    m_pBarOperationProgress->setMinimum(0);
                    m_pBarOperationProgress->setValue(0);
                    connect(m_pBarOperationProgress, SIGNAL(valueChanged(int)),
                            this, SLOT(sltProgressValueChanged(int)));
                    /* Add bar into layout: */
                    pLayoutProgress->addWidget(m_pBarOperationProgress);
                }
                /* Add stretch: */
                pLayoutProgress->addStretch();
                /* Add layout into parent: */
                pLayoutTop->addLayout(pLayoutProgress);
            }
            /* Add layout into parent: */
            pLayout->addLayout(pLayoutTop);
        }
    }
}

void UISettingsSerializerProgress::retranslateUi()
{
    /* Translate operation progress label: */
    AssertPtrReturnVoid(m_pLabelOperationProgress);
    switch (m_pSerializer->direction())
    {
        case UISettingsSerializer::Load: m_pLabelOperationProgress->setText(tr("Loading Settings...")); break;
        case UISettingsSerializer::Save: m_pLabelOperationProgress->setText(tr("Saving Settings...")); break;
    }
}

void UISettingsSerializerProgress::closeEvent(QCloseEvent *pEvent)
{
    /* No need to close the dialog: */
    pEvent->ignore();
}

void UISettingsSerializerProgress::reject()
{
    /* No need to reject the dialog. */
}

void UISettingsSerializerProgress::sltStartProcess()
{
    /* Start the serializer: */
    m_pSerializer->start();
}

void UISettingsSerializerProgress::sltAdvanceProgressValue()
{
    /* Advance the serialize progress bar: */
    AssertPtrReturnVoid(m_pBarOperationProgress);
    m_pBarOperationProgress->setValue(m_pBarOperationProgress->value() + 1);
}

void UISettingsSerializerProgress::sltProgressValueChanged(int iValue)
{
    AssertPtrReturnVoid(m_pBarOperationProgress);
    if (iValue == m_pBarOperationProgress->maximum())
        hide();
}

