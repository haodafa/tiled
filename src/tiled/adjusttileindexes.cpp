/*
 * adjusttileindexes.cpp
 * Copyright 2015, Thorbjørn Lindeijer <bjorn@lindeijer.nl>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "adjusttileindexes.h"

#include "changemapobject.h"
#include "changeproperties.h"
#include "changetileanimation.h"
#include "changetileobjectgroup.h"
#include "changetileprobability.h"
#include "changetileterrain.h"
#include "map.h"
#include "mapdocument.h"
#include "mapobject.h"
#include "objectgroup.h"
#include "painttilelayer.h"
#include "tile.h"
#include "tilelayer.h"

#include <QCoreApplication>

namespace Tiled {
namespace Internal {

AdjustTileIndexes::AdjustTileIndexes(MapDocument *mapDocument,
                                     const Tileset &tileset)
    : QUndoCommand(QCoreApplication::translate("Undo Commands",
                                               "Adjust Tile Indexes"))
{
    int oldColumnCount = tileset.expectedColumnCount();
    int newColumnCount = tileset.columnCount();

    auto isFromTileset = [&](const Cell &cell) -> bool {
        return cell.tile && cell.tile->tileset() == &tileset;
    };

    auto adjustTile = [&](const Tile *tile) -> Tile* {
        int tileIndex = tile->id();
        int row = tileIndex / oldColumnCount;
        int column = tileIndex % oldColumnCount;

        if (column < newColumnCount) {
            int newTileIndex = row * newColumnCount + column;
            return tileset.findTile(newTileIndex);
        }

        return nullptr;
    };

    QVector<MapObjectChange> objectChanges;

    // Adjust tile references from map layers
    for (Layer *layer : mapDocument->map()->layers()) {
        switch (layer->layerType()) {
        case Layer::TileLayerType: {
            TileLayer *tileLayer = static_cast<TileLayer*>(layer);
            QRegion region = tileLayer->region(isFromTileset).translated(-layer->position());

            if (!region.isEmpty()) {
                const QRect boundingRect(region.boundingRect());
                auto changedLayer = new TileLayer(QString(), 0, 0,
                                                  boundingRect.width(),
                                                  boundingRect.height());

                for (const QRect &rect : region.rects()) {
                    for (int x = rect.left(); x <= rect.right(); ++x) {
                        for (int y = rect.top(); y <= rect.bottom(); ++y) {
                            Cell cell = tileLayer->cellAt(x, y);
                            cell.tile = adjustTile(cell.tile);

                            changedLayer->setCell(x - boundingRect.x(),
                                                  y - boundingRect.y(),
                                                  cell);
                        }
                    }
                }

                new PaintTileLayer(mapDocument, tileLayer,
                                   boundingRect.x() + tileLayer->x(),
                                   boundingRect.y() + tileLayer->y(),
                                   changedLayer,
                                   this);

                delete changedLayer;
            }

            break;
        }

        case Layer::ObjectGroupType:
            for (MapObject *mapObject : *static_cast<ObjectGroup*>(layer)) {
                if (isFromTileset(mapObject->cell())) {
                    MapObjectChange change;
                    change.object = mapObject;
                    change.tile = adjustTile(mapObject->cell().tile);;
                    objectChanges.append(change);
                }
            }
            break;

        case Layer::ImageLayerType:
            break;
        }
    }

    if (!objectChanges.isEmpty()) {
        new ChangeMapObjects(mapDocument,
                             objectChanges,
                             ChangeMapObjects::ChangeTile,
                             this);
    }
}

AdjustTileMetaData::AdjustTileMetaData(MapDocument *mapDocument,
                                       const Tileset &tileset)
    : QUndoCommand(QCoreApplication::translate("Undo Commands",
                                               "Adjust Tile Indexes"))
{
    int oldColumnCount = tileset.expectedColumnCount();
    int newColumnCount = tileset.columnCount();

    auto adjustTile = [&](const Tile *tile) -> Tile* {
        int tileIndex = tile->id();
        int row = tileIndex / oldColumnCount;
        int column = tileIndex % oldColumnCount;

        if (column < newColumnCount) {
            int newTileIndex = row * newColumnCount + column;
            return tileset.findTile(newTileIndex);
        }

        return nullptr;
    };

    // Adjust tile meta data
    QList<Tile*> tilesChangingProbability;
    QList<float> tileProbabilities;
    ChangeTileTerrain::Changes terrainChanges;
    QSet<Tile*> tilesToReset;

    auto adjustAnimationFrames = [&](const QVector<Frame> &frames) -> QVector<Frame> {
        QVector<Frame> newFrames;
        for (const Frame &frame : frames) {
            if (Tile *tile = tileset.findTile(frame.tileId)) {
                if (Tile *newTile = adjustTile(tile))
                    newFrames.append(Frame { newTile->id(), frame.duration });
            }
        }
        return newFrames;
    };

    auto applyMetaData = [&](Tile *toTile,
                             const Properties &properties,
                             unsigned terrain,
                             float probability,
                             ObjectGroup *objectGroup,
                             const QVector<Frame> &frames)
    {
        if (properties != toTile->properties()) {
            new ChangeProperties(mapDocument,
                                 QCoreApplication::translate("MapDocument", "Tile"),
                                 toTile,
                                 properties,
                                 this);
        }

        if (terrain != toTile->terrain()) {
            terrainChanges.insert(toTile, ChangeTileTerrain::Change(toTile->terrain(),
                                                                    terrain));
        }

        if (probability != toTile->probability()) {
            tilesChangingProbability.append(toTile);
            tileProbabilities.append(probability);
        }

        if (objectGroup != toTile->objectGroup())
            new ChangeTileObjectGroup(mapDocument, toTile, objectGroup, this);

        if (frames != toTile->frames())
            new ChangeTileAnimation(mapDocument, toTile, frames, this);
    };

    auto moveMetaData = [&](Tile *fromTile) {
        Tile *toTile = adjustTile(fromTile);
        if (toTile == fromTile)
            return;

        tilesToReset.insert(fromTile);  // We may still need to reset "fromTile"

        if (!toTile)
            return;

        tilesToReset.remove(toTile);    // "toTile" no longer needs to get reset

        // Copy meta data from "fromTile" to "toTile"

        ObjectGroup *objectGroup = nullptr;
        if (fromTile->objectGroup())
            objectGroup = static_cast<ObjectGroup*>(fromTile->objectGroup()->clone());

        applyMetaData(toTile,
                      fromTile->properties(),
                      fromTile->terrain(),
                      fromTile->probability(),
                      objectGroup,
                      adjustAnimationFrames(fromTile->frames()));
    };

    QMapIterator<int, Tile *> iterator{tileset.tiles()};

    if (newColumnCount > oldColumnCount) {
        // Increasing column count means information is copied to higher tiles,
        // so we need to iterate backwards.
        iterator.toBack();
        while (iterator.hasPrevious())
            moveMetaData(iterator.previous().value());
    } else {
        while (iterator.hasNext())
            moveMetaData(iterator.next().value());
    }

    // Reset meta data on tiles that nothing was copied to
    QSetIterator<Tile*> resetIterator(tilesToReset);
    while (resetIterator.hasNext()) {
        applyMetaData(resetIterator.next(),
                      Properties(), -1, 1.0f, nullptr, QVector<Frame>());
    }

    if (!tilesChangingProbability.isEmpty()) {
        new ChangeTileProbability(mapDocument,
                                  tilesChangingProbability,
                                  tileProbabilities,
                                  this);
    }

    if (!terrainChanges.isEmpty())
        new ChangeTileTerrain(mapDocument, terrainChanges, this);
}

} // namespace Internal
} // namespace Tiled