/* $Id$ */
/** @file
 * VBox Qt GUI - UISoftKeyboard class implementation.
 */

/*
 * Copyright (C) 2016-2019 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

/* Qt includes: */
#include <QApplication>
#include <QFile>
#include <QToolButton>
#include <QXmlStreamReader>
#include <QVBoxLayout>

/* GUI includes: */
#include "QIDialogButtonBox.h"
#include "UIExtraDataManager.h"
#include "UISession.h"
#include "UISoftKeyboard.h"
#include "VBoxGlobal.h"

/* COM includes: */
#include "CGuest.h"
#include "CEventSource.h"

enum UIKeyState
{
    UIKeyState_NotPressed,
    UIKeyState_Pressed,
    UIKeyState_Locked,
    UIKeyState_Max
};

enum UIKeyType
{
    UIKeyType_SingleState,
    UIKeyType_DualState,
    UIKeyType_TriState,
    UIKeyType_Max
};

struct SoftKeyboardKey
{
    int       m_iWidth;
    QString   m_strLabel;
    LONG      m_scanCode;
    LONG      m_scanCodePrefix;
    int       m_iSpaceAfter;
    UIKeyType m_enmType;
};

struct SoftKeyboardRow
{
    int m_iHeight;
    int m_iStartingSpace;
    QList<SoftKeyboardKey> m_keys;
};

struct SoftKeyboardLayout
{
    QList<SoftKeyboardRow> m_rows;
};

/*********************************************************************************************************************************
*   UIKeyboardLayoutReader definition.                                                                                  *
*********************************************************************************************************************************/

class UIKeyboardLayoutReader
{

public:

    bool parseXMLFile(const QString &strFileName, SoftKeyboardLayout &layout);

private:

    bool parseRow(SoftKeyboardLayout &layout);
    bool parseSpace(SoftKeyboardRow &row);
    bool parseKey(SoftKeyboardRow &row);
    QXmlStreamReader m_xmlReader;
};


/*********************************************************************************************************************************
*   UISoftKeyboardKey definition.                                                                                  *
*********************************************************************************************************************************/

class UISoftKeyboardKey : public QToolButton
{
    Q_OBJECT;

signals:

    void sigStateChanged();

public:

    UISoftKeyboardKey(QWidget *pParent = 0);

    void setWidth(int iWidth);
    int width() const;

    void setScanCode(LONG scanCode);
    LONG scanCode() const;

    void setScanCodePrefix(LONG scanCode);
    LONG scanCodePrefix() const;

    void setSpaceAfter(int iSpace);
    int spaceAfter() const;

    void setType(UIKeyType enmType);
    UIKeyType type() const;

    UIKeyState state() const;
    QVector<LONG> scanCodeWithPrefix() const;
    void updateFontSize(float multiplier);

    void release();

protected:

    virtual void mousePressEvent(QMouseEvent *pEvent) /* override */;

private:

    void updateState(bool fPressed);
    void updateBackground();

    int   m_iWidth;
    int   m_iDefaultPixelSize;
    int   m_iDefaultPointSize;
    int   m_iSpaceAfter;
    LONG  m_scanCode;
    LONG  m_scanCodePrefix;
    UIKeyType m_enmType;
    UIKeyState m_enmState;
    QPalette     m_defaultPalette;
};

/*********************************************************************************************************************************
*   UISoftKeyboardRow definition.                                                                                  *
*********************************************************************************************************************************/

class UISoftKeyboardRow : public QWidget
{
    Q_OBJECT;

public:

    UISoftKeyboardRow(QWidget *pParent = 0);
    void updateLayout();
    int m_iWidth;
    int m_iHeight;
    QVector<UISoftKeyboardKey*> m_keys;
};

/*********************************************************************************************************************************
*   UIKeyboardLayoutReader implementation.                                                                                  *
*********************************************************************************************************************************/

bool UIKeyboardLayoutReader::parseXMLFile(const QString &strFileName, SoftKeyboardLayout &layout)
{
    QFile xmlFile(strFileName);
    if (!xmlFile.exists())
        return false;

    if (!xmlFile.open(QIODevice::ReadOnly))
        return false;

    m_xmlReader.setDevice(&xmlFile);

    if (!m_xmlReader.readNextStartElement() || m_xmlReader.name() != "layout")
        return false;

    while (m_xmlReader.readNextStartElement())
    {
        if (m_xmlReader.name() == "row")
            parseRow(layout);
        else
            m_xmlReader.skipCurrentElement();
    }

    return true;
}

bool UIKeyboardLayoutReader::parseRow(SoftKeyboardLayout &layout)
{
    layout.m_rows.append(SoftKeyboardRow());
    SoftKeyboardRow &row = layout.m_rows.last();
    row.m_iHeight = 0;
    row.m_iStartingSpace = 0;
    while (m_xmlReader.readNextStartElement())
    {
        if (m_xmlReader.name() == "key")
            parseKey(row);
        else if (m_xmlReader.name() == "space")
            parseSpace(row);
        else if (m_xmlReader.name() == "height")
            row.m_iHeight = m_xmlReader.readElementText().toInt();
        else
            m_xmlReader.skipCurrentElement();
    }
    return true;
}

bool UIKeyboardLayoutReader::parseSpace(SoftKeyboardRow &row)
{
    while (m_xmlReader.readNextStartElement())
    {
        if (m_xmlReader.name() == "width")
        {
            int iSpace = m_xmlReader.readElementText().toInt();
            /* Find the key that comes before this space (if any): */
            if (!row.m_keys.isEmpty())
                row.m_keys.back().m_iSpaceAfter = iSpace;
            /* If there are no keys yet, start the row with a space: */
            else
                row.m_iStartingSpace = iSpace;
        }
        else
            m_xmlReader.skipCurrentElement();
    }
    return true;
}

bool UIKeyboardLayoutReader::parseKey(SoftKeyboardRow &row)
{
    row.m_keys.append(SoftKeyboardKey());
    SoftKeyboardKey &key = row.m_keys.last();
    key.m_scanCode = 0;
    key.m_scanCodePrefix = 0;
    key.m_iSpaceAfter = 0;
    key.m_enmType = UIKeyType_SingleState;

    while (m_xmlReader.readNextStartElement())
    {
        if (m_xmlReader.name() == "width")
            key.m_iWidth = m_xmlReader.readElementText().toInt();
        else if (m_xmlReader.name() == "label")
        {
            if (key.m_strLabel.isEmpty())
                key.m_strLabel = m_xmlReader.readElementText();
            else
                key.m_strLabel = QString("%1%2%3").arg(key.m_strLabel).arg("\n").arg(m_xmlReader.readElementText());
        }
        else if (m_xmlReader.name() == "scancode")
        {
            QString strCode = m_xmlReader.readElementText();
            bool fOk = false;
            key.m_scanCode = strCode.toInt(&fOk, 16);
            if (!fOk)
                key.m_scanCode = 0;
        }
        else if (m_xmlReader.name() == "scancodeprefix")
        {
            QString strCode = m_xmlReader.readElementText();
            bool fOk = false;
            key.m_scanCodePrefix = strCode.toInt(&fOk, 16);
            if (!fOk)
                key.m_scanCodePrefix = 0;
        }
        else if (m_xmlReader.name() == "type")
        {
            QString strType = m_xmlReader.readElementText();
            if (strType == "tristate")
                key.m_enmType = UIKeyType_TriState;
            else if (strType == "dualstate")
                key.m_enmType = UIKeyType_DualState;
        }
        else
            m_xmlReader.skipCurrentElement();
    }
    return true;
}

/*********************************************************************************************************************************
*   UISoftKeyboardKey implementation.                                                                                  *
*********************************************************************************************************************************/

UISoftKeyboardKey::UISoftKeyboardKey(QWidget *pParent /* = 0 */)
    :QToolButton(pParent)
    , m_iWidth(1)
    , m_iSpaceAfter(0)
    , m_scanCode(0)
    , m_scanCodePrefix(0)
    , m_enmType(UIKeyType_SingleState)
    , m_enmState(UIKeyState_NotPressed)
{
    m_iDefaultPointSize = font().pointSize();
    m_iDefaultPixelSize = font().pixelSize();
    m_defaultPalette = palette();
}

void UISoftKeyboardKey::setWidth(int iWidth)
{
    m_iWidth = iWidth;
}

int UISoftKeyboardKey::width() const
{
    return m_iWidth;
}

void UISoftKeyboardKey::setScanCode(LONG scanCode)
{
    m_scanCode = scanCode;
}

LONG UISoftKeyboardKey::scanCode() const
{
    return m_scanCode;
}

void UISoftKeyboardKey::setScanCodePrefix(LONG scanCodePrefix)
{
    m_scanCodePrefix = scanCodePrefix;
}

LONG UISoftKeyboardKey::scanCodePrefix() const
{
    return m_scanCodePrefix;
}

void UISoftKeyboardKey::setSpaceAfter(int iSpace)
{
    m_iSpaceAfter = iSpace;
}

int UISoftKeyboardKey::spaceAfter() const
{
    return m_iSpaceAfter;
}

void UISoftKeyboardKey::setType(UIKeyType enmType)
{
    m_enmType = enmType;
}

UIKeyType UISoftKeyboardKey::type() const
{
    return m_enmType;
}

UIKeyState UISoftKeyboardKey::state() const
{
    return m_enmState;
}

void UISoftKeyboardKey::updateFontSize(float multiplier)
{
    if (m_iDefaultPointSize != -1)
    {
        QFont newFont = font();
        newFont.setPointSize(multiplier * m_iDefaultPointSize);
        setFont(newFont);
    }
    else
    {
        QFont newFont = font();
        newFont.setPixelSize(multiplier * m_iDefaultPixelSize);
        setFont(newFont);
    }
}

void UISoftKeyboardKey::release()
{
    updateState(false);
}

void UISoftKeyboardKey::mousePressEvent(QMouseEvent *pEvent)
{
    QToolButton::mousePressEvent(pEvent);
    updateState(true);
}

// void UISoftKeyboardKey::mouseReleaseEvent(QMouseEvent *pEvent)
// {
//     if (m_enmType == UIKeyType_SingleState)
//     {
//         QToolButton::mouseReleaseEvent(pEvent);
//         setState(UIKeyState_NotPressed)
//     }
//     else
//     {
//         pEvent->accept();
//     }

// }

void UISoftKeyboardKey::updateState(bool fPressed)
{
    if (m_enmType == UIKeyType_TriState)
    {
        if (fPressed)
        {
            if (m_enmState == UIKeyState_NotPressed)
                m_enmState = UIKeyState_Pressed;
            else if(m_enmState == UIKeyState_Pressed)
                m_enmState = UIKeyState_Locked;
            else
                m_enmState = UIKeyState_NotPressed;
        }
        else
        {
            if(m_enmState == UIKeyState_Pressed)
                m_enmState = UIKeyState_NotPressed;
        }
    }
    else if (m_enmType == UIKeyType_DualState)
    {
        if (fPressed)
        {
            if (m_enmState == UIKeyState_NotPressed)
                 m_enmState = UIKeyState_Pressed;
            else
                m_enmState = UIKeyState_NotPressed;
        }
    }
    emit sigStateChanged();
    updateBackground();
}

 void UISoftKeyboardKey::updateBackground()
 {
     if (m_enmState == UIKeyState_NotPressed)
     {
         setPalette(m_defaultPalette);
         return;
     }
     QColor newColor;

     if (m_enmState == UIKeyState_Pressed)
         newColor = QColor(255, 0, 0);
     else
         newColor = QColor(0, 255, 0);

     setAutoFillBackground(true);
     QPalette currentPalette = palette();
     currentPalette.setColor(QPalette::Button, newColor);
     currentPalette.setColor(QPalette::ButtonText, newColor);
     setPalette(currentPalette);
     update();
 }

 /*********************************************************************************************************************************
  *   UISoftKeyboardRow implementation.                                                                                  *
*********************************************************************************************************************************/

UISoftKeyboardRow::UISoftKeyboardRow(QWidget *pParent /* = 0 */)
    :QWidget(pParent)
    , m_iWidth(0)
    , m_iHeight(0)

{
}

void UISoftKeyboardRow::updateLayout()
{
    if (m_iHeight == 0)
        return;

    float fMultiplier = height() / (float)m_iHeight;
    int iX = 0;
    for (int i = 0; i < m_keys.size(); ++i)
    {
        UISoftKeyboardKey *pKey = m_keys[i];
        if (!pKey)
            continue;
        pKey->setVisible(true);
        pKey->updateFontSize(fMultiplier);
        int iKeyWidth = fMultiplier * pKey->width();
        if (i != m_keys.size() - 1)
            pKey->setGeometry(iX, 0, iKeyWidth, height());
        else
        {
            pKey->setGeometry(iX, 0, width() - iX - 1, height());
        }
        iX += fMultiplier * pKey->width();
        iX += fMultiplier * pKey->spaceAfter();
    }
}

/*********************************************************************************************************************************
*   UISoftKeyboard implementation.                                                                                  *
*********************************************************************************************************************************/

UISoftKeyboard::UISoftKeyboard(EmbedTo /* enmEmbedding */, QWidget *pParent,
                               UISession *pSession, QString strMachineName /* = QString()*/,
                               bool fShowToolbar /* = false */)
    :QIWithRetranslateUI<QWidget>(pParent)
    , m_pSession(pSession)
    , m_pMainLayout(0)
    , m_pContainerWidget(0)
    , m_pToolBar(0)
    , m_fShowToolbar(fShowToolbar)
    , m_strMachineName(strMachineName)
    , m_iTotalRowHeight(0)
    , m_iMaxRowWidth(0)
{
    prepareObjects();
    parseLayout();
    prepareConnections();
    prepareToolBar();
    loadSettings();
    retranslateUi();
}

UISoftKeyboard::~UISoftKeyboard()
{
    saveSettings();
}

void UISoftKeyboard::retranslateUi()
{
}

void UISoftKeyboard::resizeEvent(QResizeEvent *pEvent)
{
    QIWithRetranslateUI<QWidget>::resizeEvent(pEvent);
    updateLayout();
}

void UISoftKeyboard::sltHandleKeyPress()
{
    UISoftKeyboardKey *pKey = qobject_cast<UISoftKeyboardKey*>(sender());
    if (!pKey)
        return;
    if (pKey->type() != UIKeyType_SingleState)
        return;

    QVector<LONG> sequence;

    /* Add the pressed modifiers first: */
    for (int i = 0; i < m_pressedModifiers.size(); ++i)
    {
        UISoftKeyboardKey *pModifier = m_pressedModifiers[i];
        if (pModifier->scanCodePrefix() != 0)
            sequence << pModifier->scanCodePrefix();
        sequence << pModifier->scanCode();
    }

    if (pKey->scanCodePrefix() != 0)
        sequence << pKey->scanCodePrefix();
    sequence << pKey->scanCode();
    keyboard().PutScancodes(sequence);
}

void UISoftKeyboard::sltHandleKeyRelease()
{
    UISoftKeyboardKey *pKey = qobject_cast<UISoftKeyboardKey*>(sender());
    if (!pKey)
        return;

    if (pKey->type() != UIKeyType_SingleState)
        return;

    QVector<LONG> sequence;
    if (pKey->scanCodePrefix() != 0)
        sequence <<  pKey->scanCodePrefix();
    sequence << (pKey->scanCode() | 0x80);

    /* Add the pressed modifiers in the reverse order: */
    for (int i = m_pressedModifiers.size() - 1; i >= 0; --i)
    {
        UISoftKeyboardKey *pModifier = m_pressedModifiers[i];
        if (pModifier->scanCodePrefix() != 0)
            sequence << pModifier->scanCodePrefix();
        sequence << (pModifier->scanCode() | 0x80);
        /* Release the pressed modifiers (if there are not locked): */
        pModifier->release();
    }
    keyboard().PutScancodes(sequence);
}

void UISoftKeyboard::sltHandleModifierStateChange()
{
    UISoftKeyboardKey *pKey = qobject_cast<UISoftKeyboardKey*>(sender());
    if (!pKey)
        return;
    if (pKey->type() == UIKeyType_SingleState)
        return;
    if (pKey->state() == UIKeyState_NotPressed)
    {
        m_pressedModifiers.removeOne(pKey);
    }
    else
    {
        if (!m_pressedModifiers.contains(pKey))
            m_pressedModifiers.append(pKey);
    }
}

void UISoftKeyboard::prepareObjects()
{
    setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    /* Create layout: */

    m_pMainLayout = new QHBoxLayout(this);
    if (!m_pMainLayout)
        return;
    m_pContainerWidget = new QWidget;
    if (!m_pContainerWidget)
        return;
    m_pContainerWidget->setSizePolicy(QSizePolicy::MinimumExpanding, QSizePolicy::MinimumExpanding);
    m_pMainLayout->addWidget(m_pContainerWidget);
}

void UISoftKeyboard::prepareConnections()
{
}

void UISoftKeyboard::prepareToolBar()
{
}

void UISoftKeyboard::saveSettings()
{
}

void UISoftKeyboard::loadSettings()
{
}

void UISoftKeyboard::parseLayout()
{
    UIKeyboardLayoutReader reader;
    SoftKeyboardLayout layout;
    if (!reader.parseXMLFile(":/us_layout.xml", layout))
        return;

    m_iTotalRowHeight = 0;
    m_iMaxRowWidth = 0;
    qDeleteAll(m_rows);
    m_rows.clear();

    for (int i = 0; i < layout.m_rows.size(); ++i)
    {
        UISoftKeyboardRow *pNewRow = new UISoftKeyboardRow(m_pContainerWidget);
        m_rows.push_back(pNewRow);
        pNewRow->m_iHeight = layout.m_rows[i].m_iHeight;
        m_iTotalRowHeight += layout.m_rows[i].m_iHeight;
        pNewRow->m_iWidth = 0;
        for (int j = 0; j < layout.m_rows[i].m_keys.size(); ++j)
        {
            const SoftKeyboardKey &key = layout.m_rows[i].m_keys[j];
            pNewRow->m_iWidth += key.m_iWidth;
            pNewRow->m_iWidth += key.m_iSpaceAfter;
            UISoftKeyboardKey *pKey = new UISoftKeyboardKey(pNewRow);
            if (!pKey)
                continue;
            connect(pKey, &UISoftKeyboardKey::pressed, this, &UISoftKeyboard::sltHandleKeyPress);
            connect(pKey, &UISoftKeyboardKey::released, this, &UISoftKeyboard::sltHandleKeyRelease);
            connect(pKey, &UISoftKeyboardKey::sigStateChanged, this, &UISoftKeyboard::sltHandleModifierStateChange);
            pNewRow->m_keys.append(pKey);
            pKey->setText(key.m_strLabel);
            pKey->setWidth(key.m_iWidth);
            pKey->setScanCode(key.m_scanCode);
            pKey->setScanCodePrefix(key.m_scanCodePrefix);
            pKey->setSpaceAfter(key.m_iSpaceAfter);
            pKey->setType(key.m_enmType);
            pKey->hide();
        }
        m_iMaxRowWidth = qMax(m_iMaxRowWidth, pNewRow->m_iWidth);
    }
}

void UISoftKeyboard::updateLayout()
{
    if (!m_pContainerWidget)
        return;

    QSize containerSize(m_pContainerWidget->size());
    if (containerSize.width() == 0 || containerSize.height() == 0)
        return;
    float fMultiplier = containerSize.width() / (float) m_iMaxRowWidth;

    if (fMultiplier * m_iTotalRowHeight > containerSize.height())
        fMultiplier = containerSize.height() / (float) m_iTotalRowHeight;

    int y = 0;
    for (int i = 0; i < m_rows.size(); ++i)
    {
        UISoftKeyboardRow *pRow = m_rows[i];
        if (!pRow)
            continue;
        pRow->setGeometry(0, y, fMultiplier * pRow->m_iWidth, fMultiplier * pRow->m_iHeight);
        pRow->setVisible(true);
        y += fMultiplier * pRow->m_iHeight;
         pRow->updateLayout();
    }
    update();
}

CKeyboard& UISoftKeyboard::keyboard() const
{
    return m_pSession->keyboard();
}

#include "UISoftKeyboard.moc"
