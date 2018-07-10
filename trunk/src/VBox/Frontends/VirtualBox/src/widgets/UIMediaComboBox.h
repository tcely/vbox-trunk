/* $Id$ */
/** @file
 * VBox Qt GUI - UIMediaComboBox class declaration.
 */

/*
 * Copyright (C) 2006-2018 Oracle Corporation
 *
 * This file is part of VirtualBox Open Source Edition (OSE), as
 * available from http://www.virtualbox.org. This file is free software;
 * you can redistribute it and/or modify it under the terms of the GNU
 * General Public License (GPL) as published by the Free Software
 * Foundation, in version 2 as it comes in the "COPYING" file of the
 * VirtualBox OSE distribution. VirtualBox OSE is distributed in the
 * hope that it will be useful, but WITHOUT ANY WARRANTY of any kind.
 */

#ifndef ___UIMediaComboBox_h___
#define ___UIMediaComboBox_h___

/* Qt includes: */
#include <QComboBox>
#include <QString>

/* GUI includes: */
#include "VBoxGlobal.h"
#include "UILibraryDefs.h"

/** QComboBox subclass representing a list of registered media. */
class SHARED_LIBRARY_STUFF UIMediaComboBox : public QComboBox
{
    Q_OBJECT;

public:

    /** Base-to-diff media map. */
    typedef QMap<QString, QString> BaseToDiffMap;

    /** Constructs media combo-box passing @a pParent to the base-class. */
    UIMediaComboBox(QWidget *pParent);

    /** Performs refresh. */
    void refresh();
    /** Performs repopulation. */
    void repopulate();

    /** Defines @a enmMediaType. */
    void setType(UIMediumType enmMediaType) { m_enmMediaType = enmMediaType; }
    /** Returns media type. */
    UIMediumType type() const { return m_enmMediaType; }

    /** Defines @a strMachineId. */
    void setMachineId(const QString &strMachineId) { m_strMachineId = strMachineId; }

    /** Defines current item through @a strItemId. */
    void setCurrentItem(const QString &strItemId);

    /** Returns id of item with certain @a iIndex. */
    QString id(int iIndex = -1) const;
    /** Returns location of item with certain @a iIndex. */
    QString location(int iIndex = -1) const;

protected slots:

    /** Habdles medium-created signal for medium with @a strMediumId. */
    void sltHandleMediumCreated(const QString &strMediumId);
    /** Habdles medium-enumerated signal for medium with @a strMediumId. */
    void sltHandleMediumEnumerated(const QString &strMediumId);
    /** Habdles medium-deleted signal for medium with @a strMediumId. */
    void sltHandleMediumDeleted(const QString &strMediumId);

    /** Handles medium enumeration start. */
    void sltHandleMediumEnumerationStart();

    /** Handles combo activation for item with certain @a iIndex. */
    void sltHandleComboActivated(int iIndex);

    /** Handles combo hovering for item with certain @a index. */
    void sltHandleComboHovered(const QModelIndex &index);

protected:

    /** Prepares all. */
    void prepare();

    /** Uses the tool-tip of the item with @a iIndex. */
    void updateToolTip(int iIndex);

    /** Appends item for certain @a guiMedium. */
    void appendItem(const UIMedium &guiMedium);
    /** Replases item on certain @a iPosition with new item based on @a guiMedium. */
    void replaceItem(int iPosition, const UIMedium &guiMedium);

    /** Searches for a @a iIndex of medium with certain @a strId. */
    bool findMediaIndex(const QString &strId, int &iIndex);

    /** Holds the media type. */
    UIMediumType  m_enmMediaType;

    /** Holds the machine ID. */
    QString  m_strMachineId;

    /** Simplified media description. */
    struct Medium
    {
        Medium() {}
        Medium(const QString &strId,
               const QString &strLocation,
               const QString &strToolTip)
            : id(strId), location(strLocation), toolTip(strToolTip)
        {}

        QString  id;
        QString  location;
        QString  toolTip;
    };
    /** Vector of simplified media descriptions. */
    typedef QVector<Medium> Media;

    /** Holds currently cached media descriptions. */
    Media  m_media;

    /** Holds the last chosen medium ID. */
    QString  m_strLastItemId;
};

#endif /* !___UIMediaComboBox_h___ */