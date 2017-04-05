/* $Id$ */
/** @file
 * VBox Qt GUI - UIMachineSettingsGeneral class implementation.
 */

/*
 * Copyright (C) 2006-2017 Oracle Corporation
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
# include <QDir>
# include <QLineEdit>

/* GUI includes: */
# include "QIWidgetValidator.h"
# include "UIConverter.h"
# include "UIMachineSettingsGeneral.h"
# include "UIMessageCenter.h"
# include "UIModalWindowManager.h"
# include "UIProgressDialog.h"

/* COM includes: */
# include "CExtPack.h"
# include "CExtPackManager.h"
# include "CMedium.h"
# include "CMediumAttachment.h"

#endif /* !VBOX_WITH_PRECOMPILED_HEADERS */


/** Machine settings: General page data structure. */
struct UIDataSettingsMachineGeneral
{
    /** Constructs data. */
    UIDataSettingsMachineGeneral()
        : m_strName(QString())
        , m_strGuestOsTypeId(QString())
        , m_strSnapshotsFolder(QString())
        , m_strSnapshotsHomeDir(QString())
        , m_clipboardMode(KClipboardMode_Disabled)
        , m_dndMode(KDnDMode_Disabled)
        , m_strDescription(QString())
        , m_fEncryptionEnabled(false)
        , m_fEncryptionCipherChanged(false)
        , m_fEncryptionPasswordChanged(false)
        , m_iEncryptionCipherIndex(-1)
        , m_strEncryptionPassword(QString())
    {}

    /** Returns whether the @a other passed data is equal to this one. */
    bool equal(const UIDataSettingsMachineGeneral &other) const
    {
        return true
               && (m_strName == other.m_strName)
               && (m_strGuestOsTypeId == other.m_strGuestOsTypeId)
               && (m_strSnapshotsFolder == other.m_strSnapshotsFolder)
               && (m_strSnapshotsHomeDir == other.m_strSnapshotsHomeDir)
               && (m_clipboardMode == other.m_clipboardMode)
               && (m_dndMode == other.m_dndMode)
               && (m_strDescription == other.m_strDescription)
               && (m_fEncryptionEnabled == other.m_fEncryptionEnabled)
               && (m_fEncryptionCipherChanged == other.m_fEncryptionCipherChanged)
               && (m_fEncryptionPasswordChanged == other.m_fEncryptionPasswordChanged)
               ;
    }

    /** Returns whether the @a other passed data is equal to this one. */
    bool operator==(const UIDataSettingsMachineGeneral &other) const { return equal(other); }
    /** Returns whether the @a other passed data is different from this one. */
    bool operator!=(const UIDataSettingsMachineGeneral &other) const { return !equal(other); }

    /** Holds the VM name. */
    QString  m_strName;
    /** Holds the VM OS type ID. */
    QString  m_strGuestOsTypeId;

    /** Holds the VM snapshot folder. */
    QString         m_strSnapshotsFolder;
    /** Holds the default VM snapshot folder. */
    QString         m_strSnapshotsHomeDir;
    /** Holds the VM shared clipboard mode. */
    KClipboardMode  m_clipboardMode;
    /** Holds the VM drag&drop mode. */
    KDnDMode        m_dndMode;

    /** Holds the VM description. */
    QString  m_strDescription;

    /** Holds whether the encryption is enabled. */
    bool                   m_fEncryptionEnabled;
    /** Holds whether the encryption cipher was changed. */
    bool                   m_fEncryptionCipherChanged;
    /** Holds whether the encryption password was changed. */
    bool                   m_fEncryptionPasswordChanged;
    /** Holds the encryption cipher index. */
    int                    m_iEncryptionCipherIndex;
    /** Holds the encryption password. */
    QString                m_strEncryptionPassword;
    /** Holds the encrypted medium ids. */
    EncryptedMediumMap     m_encryptedMediums;
    /** Holds the encryption passwords. */
    EncryptionPasswordMap  m_encryptionPasswords;
};


UIMachineSettingsGeneral::UIMachineSettingsGeneral()
    : m_fHWVirtExEnabled(false)
    , m_fEncryptionCipherChanged(false)
    , m_fEncryptionPasswordChanged(false)
    , m_pCache(0)
{
    /* Prepare: */
    prepare();
}

UIMachineSettingsGeneral::~UIMachineSettingsGeneral()
{
    /* Cleanup: */
    cleanup();
}

CGuestOSType UIMachineSettingsGeneral::guestOSType() const
{
    AssertPtrReturn(m_pNameAndSystemEditor, CGuestOSType());
    return m_pNameAndSystemEditor->type();
}

bool UIMachineSettingsGeneral::is64BitOSTypeSelected() const
{
    AssertPtrReturn(m_pNameAndSystemEditor, false);
    return m_pNameAndSystemEditor->type().GetIs64Bit();
}

#ifdef VBOX_WITH_VIDEOHWACCEL
bool UIMachineSettingsGeneral::isWindowsOSTypeSelected() const
{
    AssertPtrReturn(m_pNameAndSystemEditor, false);
    return m_pNameAndSystemEditor->type().GetFamilyId() == "Windows";
}
#endif /* VBOX_WITH_VIDEOHWACCEL */

void UIMachineSettingsGeneral::setHWVirtExEnabled(bool fEnabled)
{
    /* Make sure hardware virtualization extension has changed: */
    if (m_fHWVirtExEnabled == fEnabled)
        return;

    /* Update hardware virtualization extension value: */
    m_fHWVirtExEnabled = fEnabled;

    /* Revalidate: */
    revalidate();
}

bool UIMachineSettingsGeneral::changed() const
{
    return m_pCache->wasChanged();
}

void UIMachineSettingsGeneral::loadToCacheFrom(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Clear cache initially: */
    m_pCache->clear();

    /* Prepare old general data: */
    UIDataSettingsMachineGeneral oldGeneralData;

    /* Gather old 'Basic' data: */
    oldGeneralData.m_strName = m_machine.GetName();
    oldGeneralData.m_strGuestOsTypeId = m_machine.GetOSTypeId();

    /* Gather old 'Advanced' data: */
    oldGeneralData.m_strSnapshotsFolder = m_machine.GetSnapshotFolder();
    oldGeneralData.m_strSnapshotsHomeDir = QFileInfo(m_machine.GetSettingsFilePath()).absolutePath();
    oldGeneralData.m_clipboardMode = m_machine.GetClipboardMode();
    oldGeneralData.m_dndMode = m_machine.GetDnDMode();

    /* Gather old 'Description' data: */
    oldGeneralData.m_strDescription = m_machine.GetDescription();

    /* Gather old 'Encryption' data: */
    QString strCipher;
    bool fEncryptionCipherCommon = true;
    /* Prepare the map of the encrypted mediums: */
    EncryptedMediumMap encryptedMediums;
    foreach (const CMediumAttachment &attachment, m_machine.GetMediumAttachments())
    {
        /* Check hard-drive attachments only: */
        if (attachment.GetType() == KDeviceType_HardDisk)
        {
            /* Get the attachment medium base: */
            const CMedium comMedium = attachment.GetMedium();
            /* Check medium encryption attributes: */
            QString strCurrentCipher;
            const QString strCurrentPasswordId = comMedium.GetEncryptionSettings(strCurrentCipher);
            if (comMedium.isOk())
            {
                encryptedMediums.insert(strCurrentPasswordId, comMedium.GetId());
                if (strCurrentCipher != strCipher)
                {
                    if (strCipher.isNull())
                        strCipher = strCurrentCipher;
                    else
                        fEncryptionCipherCommon = false;
                }
            }
        }
    }
    oldGeneralData.m_fEncryptionEnabled = !encryptedMediums.isEmpty();
    oldGeneralData.m_fEncryptionCipherChanged = false;
    oldGeneralData.m_fEncryptionPasswordChanged = false;
    if (fEncryptionCipherCommon)
        oldGeneralData.m_iEncryptionCipherIndex = m_encryptionCiphers.indexOf(strCipher);
    if (oldGeneralData.m_iEncryptionCipherIndex == -1)
        oldGeneralData.m_iEncryptionCipherIndex = 0;
    oldGeneralData.m_encryptedMediums = encryptedMediums;

    /* Cache old general data: */
    m_pCache->cacheInitialData(oldGeneralData);

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

void UIMachineSettingsGeneral::getFromCache()
{
    /* Get old general data from the cache: */
    const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();

    /* Load old 'Basic' data to the page: */
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    m_pNameAndSystemEditor->setName(oldGeneralData.m_strName);
    m_pNameAndSystemEditor->setType(vboxGlobal().vmGuestOSType(oldGeneralData.m_strGuestOsTypeId));

    /* Load old 'Advanced' data to the page: */
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mCbDragAndDrop);
    mPsSnapshot->setPath(oldGeneralData.m_strSnapshotsFolder);
    mPsSnapshot->setHomeDir(oldGeneralData.m_strSnapshotsHomeDir);
    mCbClipboard->setCurrentIndex(oldGeneralData.m_clipboardMode);
    mCbDragAndDrop->setCurrentIndex(oldGeneralData.m_dndMode);

    /* Load old 'Description' data to the page: */
    AssertPtrReturnVoid(mTeDescription);
    mTeDescription->setPlainText(oldGeneralData.m_strDescription);

    /* Load old 'Encryption' data to the page: */
    AssertPtrReturnVoid(m_pCheckBoxEncryption);
    AssertPtrReturnVoid(m_pComboCipher);
    m_pCheckBoxEncryption->setChecked(oldGeneralData.m_fEncryptionEnabled);
    m_pComboCipher->setCurrentIndex(oldGeneralData.m_iEncryptionCipherIndex);
    m_fEncryptionCipherChanged = oldGeneralData.m_fEncryptionCipherChanged;
    m_fEncryptionPasswordChanged = oldGeneralData.m_fEncryptionPasswordChanged;

    /* Polish page finally: */
    polishPage();

    /* Revalidate: */
    revalidate();
}

void UIMachineSettingsGeneral::putToCache()
{
    /* Prepare new general data: */
    UIDataSettingsMachineGeneral newGeneralData;

    /* Gather new 'Basic' data from page: */
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    newGeneralData.m_strName = m_pNameAndSystemEditor->name();
    newGeneralData.m_strGuestOsTypeId = m_pNameAndSystemEditor->type().GetId();

    /* Gather new 'Advanced' data from page: */
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mCbDragAndDrop);
    newGeneralData.m_strSnapshotsFolder = mPsSnapshot->path();
    newGeneralData.m_clipboardMode = (KClipboardMode)mCbClipboard->currentIndex();
    newGeneralData.m_dndMode = (KDnDMode)mCbDragAndDrop->currentIndex();

    /* Gather new 'Description' data from page: */
    AssertPtrReturnVoid(mTeDescription);
    newGeneralData.m_strDescription = mTeDescription->toPlainText().isEmpty() ?
                                      QString::null : mTeDescription->toPlainText();

    /* Gather new 'Encryption' data from page: */
    AssertPtrReturnVoid(m_pCheckBoxEncryption);
    AssertPtrReturnVoid(m_pComboCipher);
    AssertPtrReturnVoid(m_pEditorEncryptionPassword);
    newGeneralData.m_fEncryptionEnabled = m_pCheckBoxEncryption->isChecked();
    newGeneralData.m_fEncryptionCipherChanged = m_fEncryptionCipherChanged;
    newGeneralData.m_fEncryptionPasswordChanged = m_fEncryptionPasswordChanged;
    newGeneralData.m_iEncryptionCipherIndex = m_pComboCipher->currentIndex();
    newGeneralData.m_strEncryptionPassword = m_pEditorEncryptionPassword->text();
    /* If encryption status, cipher or password is changed: */
    if (newGeneralData.m_fEncryptionEnabled != m_pCache->base().m_fEncryptionEnabled ||
        newGeneralData.m_fEncryptionCipherChanged != m_pCache->base().m_fEncryptionCipherChanged ||
        newGeneralData.m_fEncryptionPasswordChanged != m_pCache->base().m_fEncryptionPasswordChanged)
    {
        /* Ask for the disk encryption passwords if necessary: */
        if (!m_pCache->base().m_encryptedMediums.isEmpty())
        {
            /* Create corresponding dialog: */
            QWidget *pDlgParent = windowManager().realParentWindow(window());
            QPointer<UIAddDiskEncryptionPasswordDialog> pDlg =
                 new UIAddDiskEncryptionPasswordDialog(pDlgParent,
                                                       newGeneralData.m_strName,
                                                       newGeneralData.m_encryptedMediums);
            /* Execute it and acquire the result: */
            if (pDlg->exec() == QDialog::Accepted)
                newGeneralData.m_encryptionPasswords = pDlg->encryptionPasswords();
            /* Delete dialog if still valid: */
            if (pDlg)
                delete pDlg;
        }
    }

    /* Cache new general data: */
    m_pCache->cacheCurrentData(newGeneralData);
}

void UIMachineSettingsGeneral::saveFromCacheTo(QVariant &data)
{
    /* Fetch data to machine: */
    UISettingsPageMachine::fetchData(data);

    /* Make sure machine is in valid mode & general data was changed: */
    if (isMachineInValidMode() && m_pCache->wasChanged())
    {
        /* Get old general data from the cache: */
        const UIDataSettingsMachineGeneral &oldGeneralData = m_pCache->base();
        /* Get new general data from the cache: */
        const UIDataSettingsMachineGeneral &newGeneralData = m_pCache->data();

        /* Store machine OS type ID: */
        if (isMachineOffline() && newGeneralData.m_strGuestOsTypeId != oldGeneralData.m_strGuestOsTypeId)
        {
            m_machine.SetOSTypeId(newGeneralData.m_strGuestOsTypeId);
            // Must update long mode CPU feature bit when os type changed:
            CVirtualBox vbox = vboxGlobal().virtualBox();
            const CGuestOSType enmNewType = vbox.GetGuestOSType(newGeneralData.m_strGuestOsTypeId);
            m_machine.SetCPUProperty(KCPUPropertyType_LongMode, enmNewType.GetIs64Bit());
        }

        /* Store machine clipboard mode: */
        if (newGeneralData.m_clipboardMode != oldGeneralData.m_clipboardMode)
            m_machine.SetClipboardMode(newGeneralData.m_clipboardMode);
        /* Store machine D&D mode: */
        if (newGeneralData.m_dndMode != oldGeneralData.m_dndMode)
            m_machine.SetDnDMode(newGeneralData.m_dndMode);
        /* Store machine snapshot folder: */
        if (isMachineOffline() && newGeneralData.m_strSnapshotsFolder != oldGeneralData.m_strSnapshotsFolder)
            m_machine.SetSnapshotFolder(newGeneralData.m_strSnapshotsFolder);

        // VM name should go after the snapshot folder from the 'Advanced' data
        // as otherwise VM rename magic can collide with the snapshot folder one.
        /* Store machine name: */
        if (isMachineOffline() && newGeneralData.m_strName != oldGeneralData.m_strName)
            m_machine.SetName(newGeneralData.m_strName);

        /* Store machine description: */
        if (newGeneralData.m_strDescription != oldGeneralData.m_strDescription)
            m_machine.SetDescription(newGeneralData.m_strDescription);

        if (isMachineOffline())
        {
            /* Make sure it either encryption is changed itself,
             * or the encryption was already enabled and either cipher or password is changed. */
            if (   newGeneralData.m_fEncryptionEnabled != oldGeneralData.m_fEncryptionEnabled
                || (   oldGeneralData.m_fEncryptionEnabled
                    && (   newGeneralData.m_fEncryptionCipherChanged != oldGeneralData.m_fEncryptionCipherChanged
                        || newGeneralData.m_fEncryptionPasswordChanged != oldGeneralData.m_fEncryptionPasswordChanged)))
            {
                /* Cipher attribute changed? */
                QString strNewCipher;
                if (newGeneralData.m_fEncryptionCipherChanged)
                {
                    strNewCipher = newGeneralData.m_fEncryptionEnabled ?
                                   m_encryptionCiphers.at(newGeneralData.m_iEncryptionCipherIndex) : QString();
                }
                /* Password attribute changed? */
                QString strNewPassword;
                QString strNewPasswordId;
                if (newGeneralData.m_fEncryptionPasswordChanged)
                {
                    strNewPassword = newGeneralData.m_fEncryptionEnabled ?
                                     newGeneralData.m_strEncryptionPassword : QString();
                    strNewPasswordId = newGeneralData.m_fEncryptionEnabled ?
                                       m_machine.GetName() : QString();
                }

                /* Get the maps of encrypted mediums and their passwords: */
                const EncryptedMediumMap &encryptedMedium = newGeneralData.m_encryptedMediums;
                const EncryptionPasswordMap &encryptionPasswords = newGeneralData.m_encryptionPasswords;
                /* Enumerate attachments: */
                foreach (const CMediumAttachment &attachment, m_machine.GetMediumAttachments())
                {
                    /* Enumerate hard-drives only: */
                    if (attachment.GetType() == KDeviceType_HardDisk)
                    {
                        /* Get corresponding medium: */
                        CMedium comMedium = attachment.GetMedium();

                        /* Check if old password exists/provided: */
                        const QString strOldPasswordId = encryptedMedium.key(comMedium.GetId());
                        const QString strOldPassword = encryptionPasswords.value(strOldPasswordId);

                        /* Update encryption: */
                        CProgress cprogress = comMedium.ChangeEncryption(strOldPassword,
                                                                         strNewCipher,
                                                                         strNewPassword,
                                                                         strNewPasswordId);
                        if (!comMedium.isOk())
                        {
                            QMetaObject::invokeMethod(this, "sigOperationProgressError", Qt::BlockingQueuedConnection,
                                                      Q_ARG(QString, UIMessageCenter::formatErrorInfo(comMedium)));
                            continue;
                        }
                        UIProgress uiprogress(cprogress);
                        connect(&uiprogress, SIGNAL(sigProgressChange(ulong, QString, ulong, ulong)),
                                this, SIGNAL(sigOperationProgressChange(ulong, QString, ulong, ulong)),
                                Qt::QueuedConnection);
                        connect(&uiprogress, SIGNAL(sigProgressError(QString)),
                                this, SIGNAL(sigOperationProgressError(QString)),
                                Qt::BlockingQueuedConnection);
                        uiprogress.run(350);
                    }
                }
            }
        }
    }

    /* Upload machine to data: */
    UISettingsPageMachine::uploadData(data);
}

bool UIMachineSettingsGeneral::validate(QList<UIValidationMessage> &messages)
{
    /* Pass by default: */
    bool fPass = true;

    /* Prepare message: */
    UIValidationMessage message;

    /* 'Basic' tab validations: */
    message.first = VBoxGlobal::removeAccelMark(mTwGeneral->tabText(0));
    message.second.clear();

    /* VM name validation: */
    AssertPtrReturn(m_pNameAndSystemEditor, false);
    if (m_pNameAndSystemEditor->name().trimmed().isEmpty())
    {
        message.second << tr("No name specified for the virtual machine.");
        fPass = false;
    }

    /* OS type & VT-x/AMD-v correlation: */
    if (is64BitOSTypeSelected() && !m_fHWVirtExEnabled)
    {
        message.second << tr("The virtual machine operating system hint is set to a 64-bit type. "
                             "64-bit guest systems require hardware virtualization, "
                             "so this will be enabled automatically if you confirm the changes.");
    }

    /* Serialize message: */
    if (!message.second.isEmpty())
        messages << message;

    /* 'Encryption' tab validations: */
    message.first = VBoxGlobal::removeAccelMark(mTwGeneral->tabText(3));
    message.second.clear();

    /* Encryption validation: */
    AssertPtrReturn(m_pCheckBoxEncryption, false);
    if (m_pCheckBoxEncryption->isChecked())
    {
#ifdef VBOX_WITH_EXTPACK
        /* Encryption Extension Pack presence test: */
        const CExtPack extPack = vboxGlobal().virtualBox().GetExtensionPackManager().Find(GUI_ExtPackName);
        if (extPack.isNull() || !extPack.GetUsable())
        {
            message.second << tr("You are trying to encrypt this virtual machine. "
                                 "However, this requires the <i>%1</i> to be installed. "
                                 "Please install the Extension Pack from the VirtualBox download site.")
                                 .arg(GUI_ExtPackName);
            fPass = false;
        }
#endif /* VBOX_WITH_EXTPACK */

        /* Cipher should be chosen if once changed: */
        AssertPtrReturn(m_pComboCipher, false);
        if (!m_pCache->base().m_fEncryptionEnabled ||
            m_fEncryptionCipherChanged)
        {
            if (m_pComboCipher->currentIndex() == 0)
                message.second << tr("Encryption cipher type not specified.");
            fPass = false;
        }

        /* Password should be entered and confirmed if once changed: */
        AssertPtrReturn(m_pEditorEncryptionPassword, false);
        AssertPtrReturn(m_pEditorEncryptionPasswordConfirm, false);
        if (!m_pCache->base().m_fEncryptionEnabled ||
            m_fEncryptionPasswordChanged)
        {
            if (m_pEditorEncryptionPassword->text().isEmpty())
                message.second << tr("Encryption password empty.");
            else
            if (m_pEditorEncryptionPassword->text() !=
                m_pEditorEncryptionPasswordConfirm->text())
                message.second << tr("Encryption passwords do not match.");
            fPass = false;
        }
    }

    /* Serialize message: */
    if (!message.second.isEmpty())
        messages << message;

    /* Return result: */
    return fPass;
}

void UIMachineSettingsGeneral::setOrderAfter(QWidget *pWidget)
{
    /* 'Basic' tab: */
    AssertPtrReturnVoid(pWidget);
    AssertPtrReturnVoid(mTwGeneral);
    AssertPtrReturnVoid(mTwGeneral->focusProxy());
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    setTabOrder(pWidget, mTwGeneral->focusProxy());
    setTabOrder(mTwGeneral->focusProxy(), m_pNameAndSystemEditor);

    /* 'Advanced' tab: */
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mCbDragAndDrop);
    setTabOrder(m_pNameAndSystemEditor, mPsSnapshot);
    setTabOrder(mPsSnapshot, mCbClipboard);
    setTabOrder(mCbClipboard, mCbDragAndDrop);

    /* 'Description' tab: */
    AssertPtrReturnVoid(mTeDescription);
    setTabOrder(mCbDragAndDrop, mTeDescription);
}

void UIMachineSettingsGeneral::retranslateUi()
{
    /* Translate uic generated strings: */
    Ui::UIMachineSettingsGeneral::retranslateUi(this);

    /* Translate path selector: */
    AssertPtrReturnVoid(mPsSnapshot);
    mPsSnapshot->setWhatsThis(tr("Holds the path where snapshots of this "
                                 "virtual machine will be stored. Be aware that "
                                 "snapshots can take quite a lot of storage space."));
    /* Translate Shared Clipboard mode combo: */
    AssertPtrReturnVoid(mCbClipboard);
    mCbClipboard->setItemText(0, gpConverter->toString(KClipboardMode_Disabled));
    mCbClipboard->setItemText(1, gpConverter->toString(KClipboardMode_HostToGuest));
    mCbClipboard->setItemText(2, gpConverter->toString(KClipboardMode_GuestToHost));
    mCbClipboard->setItemText(3, gpConverter->toString(KClipboardMode_Bidirectional));
    /* Translate Drag'n'drop mode combo: */
    AssertPtrReturnVoid(mCbDragAndDrop);
    mCbDragAndDrop->setItemText(0, gpConverter->toString(KDnDMode_Disabled));
    mCbDragAndDrop->setItemText(1, gpConverter->toString(KDnDMode_HostToGuest));
    mCbDragAndDrop->setItemText(2, gpConverter->toString(KDnDMode_GuestToHost));
    mCbDragAndDrop->setItemText(3, gpConverter->toString(KDnDMode_Bidirectional));

    /* Translate Cipher type combo: */
    AssertPtrReturnVoid(m_pComboCipher);
    m_pComboCipher->setItemText(0, tr("Leave Unchanged", "cipher type"));
}

void UIMachineSettingsGeneral::polishPage()
{
    /* 'Basic' tab: */
    AssertPtrReturnVoid(m_pNameAndSystemEditor);
    m_pNameAndSystemEditor->setEnabled(isMachineOffline());

    /* 'Advanced' tab: */
    AssertPtrReturnVoid(mLbSnapshot);
    AssertPtrReturnVoid(mPsSnapshot);
    AssertPtrReturnVoid(mLbClipboard);
    AssertPtrReturnVoid(mCbClipboard);
    AssertPtrReturnVoid(mLbDragAndDrop);
    AssertPtrReturnVoid(mCbDragAndDrop);
    mLbSnapshot->setEnabled(isMachineOffline());
    mPsSnapshot->setEnabled(isMachineOffline());
    mLbClipboard->setEnabled(isMachineInValidMode());
    mCbClipboard->setEnabled(isMachineInValidMode());
    mLbDragAndDrop->setEnabled(isMachineInValidMode());
    mCbDragAndDrop->setEnabled(isMachineInValidMode());

    /* 'Description' tab: */
    AssertPtrReturnVoid(mTeDescription);
    mTeDescription->setEnabled(isMachineInValidMode());

    /* 'Encryption' tab: */
    AssertPtrReturnVoid(m_pCheckBoxEncryption);
    AssertPtrReturnVoid(m_pWidgetEncryption);
    m_pCheckBoxEncryption->setEnabled(isMachineOffline());
    m_pWidgetEncryption->setEnabled(isMachineOffline() && m_pCheckBoxEncryption->isChecked());
}

void UIMachineSettingsGeneral::prepare()
{
    /* Apply UI decorations: */
    Ui::UIMachineSettingsGeneral::setupUi(this);

    /* Prepare cache: */
    m_pCache = new UISettingsCacheMachineGeneral;
    AssertPtrReturnVoid(m_pCache);

    /* Tree-widget created in the .ui file. */
    {
        /* Prepare 'Basic' tab: */
        prepareTabBasic();
        /* Prepare 'Advanced' tab: */
        prepareTabAdvanced();
        /* Prepare 'Description' tab: */
        prepareTabDescription();
        /* Prepare 'Encryption' tab: */
        prepareTabEncryption();
        /* Prepare connections: */
        prepareConnections();
    }

    /* Apply language settings: */
    retranslateUi();
}

void UIMachineSettingsGeneral::prepareTabBasic()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Name and OS Type widget created in the .ui file. */
        AssertPtrReturnVoid(m_pNameAndSystemEditor);
        {
            /* Configure widget: */
            m_pNameAndSystemEditor->nameEditor()->setValidator(new QRegExpValidator(QRegExp(".+"), this));
        }
    }
}

void UIMachineSettingsGeneral::prepareTabAdvanced()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Shared Clipboard Mode combo-box created in the .ui file. */
        AssertPtrReturnVoid(mCbClipboard);
        {
            /* Configure combo-box: */
            mCbClipboard->addItem(""); /* KClipboardMode_Disabled */
            mCbClipboard->addItem(""); /* KClipboardMode_HostToGuest */
            mCbClipboard->addItem(""); /* KClipboardMode_GuestToHost */
            mCbClipboard->addItem(""); /* KClipboardMode_Bidirectional */
        }

        /* Drag&drop Mode combo-box created in the .ui file. */
        AssertPtrReturnVoid(mCbDragAndDrop);
        {
            /* Configure combo-box: */
            mCbDragAndDrop->addItem(""); /* KDnDMode_Disabled */
            mCbDragAndDrop->addItem(""); /* KDnDMode_HostToGuest */
            mCbDragAndDrop->addItem(""); /* KDnDMode_GuestToHost */
            mCbDragAndDrop->addItem(""); /* KDnDMode_Bidirectional */
        }
    }
}

void UIMachineSettingsGeneral::prepareTabDescription()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Description Text editor created in the .ui file. */
        AssertPtrReturnVoid(mTeDescription);
        {
            /* Configure editor: */
#ifdef VBOX_WS_MAC
            mTeDescription->setMinimumHeight(150);
#endif
        }
    }
}

void UIMachineSettingsGeneral::prepareTabEncryption()
{
    /* Tab and it's layout created in the .ui file. */
    {
        /* Encryption Cipher combo-box created in the .ui file. */
        AssertPtrReturnVoid(m_pComboCipher);
        {
            /* Configure combo-box: */
            m_encryptionCiphers << QString()
                                << "AES-XTS256-PLAIN64"
                                << "AES-XTS128-PLAIN64";
            m_pComboCipher->addItems(m_encryptionCiphers);
        }

        /* Encryption Password editor created in the .ui file. */
        AssertPtrReturnVoid(m_pEditorEncryptionPassword);
        {
            /* Configure editor: */
            m_pEditorEncryptionPassword->setEchoMode(QLineEdit::Password);
        }

        /* Encryption Password Confirmation editor created in the .ui file. */
        AssertPtrReturnVoid(m_pEditorEncryptionPasswordConfirm);
        {
            /* Configure editor: */
            m_pEditorEncryptionPasswordConfirm->setEchoMode(QLineEdit::Password);
        }
    }
}

void UIMachineSettingsGeneral::prepareConnections()
{
    /* Configure 'Basic' connections: */
    connect(m_pNameAndSystemEditor, SIGNAL(sigOsTypeChanged()),
            this, SLOT(revalidate()));
    connect(m_pNameAndSystemEditor, SIGNAL(sigNameChanged(const QString &)),
            this, SLOT(revalidate()));

    /* Configure 'Encryption' connections: */
    connect(m_pCheckBoxEncryption, SIGNAL(toggled(bool)),
            this, SLOT(revalidate()));
    connect(m_pComboCipher, SIGNAL(currentIndexChanged(int)),
            this, SLOT(sltMarkEncryptionCipherChanged()));
    connect(m_pComboCipher, SIGNAL(currentIndexChanged(int)),
            this, SLOT(revalidate()));
    connect(m_pEditorEncryptionPassword, SIGNAL(textEdited(const QString&)),
            this, SLOT(sltMarkEncryptionPasswordChanged()));
    connect(m_pEditorEncryptionPassword, SIGNAL(textEdited(const QString&)),
            this, SLOT(revalidate()));
    connect(m_pEditorEncryptionPasswordConfirm, SIGNAL(textEdited(const QString&)),
            this, SLOT(sltMarkEncryptionPasswordChanged()));
    connect(m_pEditorEncryptionPasswordConfirm, SIGNAL(textEdited(const QString&)),
            this, SLOT(revalidate()));
}

void UIMachineSettingsGeneral::cleanup()
{
    /* Cleanup cache: */
    delete m_pCache;
    m_pCache = 0;
}

