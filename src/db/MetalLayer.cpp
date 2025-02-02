#include "MetalLayer.h"

namespace db {

ostream& operator<<(ostream& os, const Track& track) {
    os << "track(lo=" << track.lowerCPIdx << ", up=" << track.upperCPIdx << ", loc=" << track.location << ")";
    return os;
}

ostream& operator<<(ostream& os, const CrossPoint& cp) {
    os << "crossPt(lo=" << cp.lowerTrackIdx << ", up=" << cp.upperTrackIdx << ", loc=" << cp.location << ")";
    return os;
}

MetalLayer::MetalLayer(Rsyn::PhysicalLayer rsynLayer,
                       const vector<Rsyn::PhysicalTracks>& rsynTracks,
                       const DBU libDBU) {
    // Rsyn::PhysicalLayer (LEF)
    lefiLayer* layer = rsynLayer.getLayer();
    name = layer->name();
    direction = strcmp(layer->direction(), "HORIZONTAL") ? X : Y;
    idx = rsynLayer.getRelativeIndex();
    width = static_cast<DBU>(std::round(layer->width() * libDBU));
    minWidth = static_cast<DBU>(std::round(layer->minwidth() * libDBU));
    widthForSuffOvlp = std::ceil(minWidth * 0.7071);
    shrinkForSuffOvlp = std::max<DBU>(0, std::ceil(widthForSuffOvlp - width * 0.5));
    minArea = static_cast<DBU>(std::round(layer->area() * libDBU * libDBU));
    minLenRaw = minArea / width - width;
    // default spacing
    const int numSpaceTable = layer->numSpacingTable();
    if (!numSpaceTable) {
        log() << "Warning in " << __func__ << ": For " << name << ", no run spacing table..." << std::endl;
    } else {
        for (int iSpaceTable = 0; iSpaceTable < numSpaceTable; ++iSpaceTable) {
            if (!layer->spacingTable(iSpaceTable)->isParallel()) {
                log() << "Warning in " << __func__ << ": For " << name << ", unidentified spacing table...\n";
                continue;
            }

            const lefiParallel* parallel = layer->spacingTable(iSpaceTable)->parallel();
            const int numLength = parallel->numLength();
            if (numLength > 0) {
                parallelLength.resize(numLength);
                for (unsigned iLength = 0; iLength != (unsigned)numLength; ++iLength) {
                    parallelLength[iLength] = static_cast<DBU>(std::round(parallel->length(iLength) * libDBU));
                }
            }
            const int numWidth = parallel->numWidth();
            if (numWidth > 0) {
                parallelWidth.resize(numWidth);
                parallelWidthSpace.resize(numWidth);
                for (unsigned iWidth = 0; iWidth != (unsigned)numWidth; ++iWidth) {
                    parallelWidth[iWidth] = static_cast<DBU>(std::round(parallel->width(iWidth) * libDBU));
                    parallelWidthSpace[iWidth].resize(std::max(1, numLength), 0);
                    for (int iLength = 0; iLength < numLength; ++iLength) {
                        parallelWidthSpace[iWidth][iLength] =
                            static_cast<DBU>(std::round(parallel->widthSpacing(iWidth, iLength) * libDBU));
                    }
                }
                defaultSpace = getParaRunSpace(width);
                paraRunSpaceForLargerWidth = (parallelWidthSpace.size() > 1) ? parallelWidthSpace[1][0] : defaultSpace;
            }
        }
    }
    //  eol spacing
    if (!layer->hasSpacingNumber()) {
        log() << "Warning in " << __func__ << ": For " << name << ", no spacing rules...\n";
    } else {
        const int numSpace = layer->numSpacing();
        spaceRules.reserve(numSpace);
        for (int iSpace = 0; iSpace < numSpace; ++iSpace) {
            const DBU space{std::lround(layer->spacing(iSpace) * libDBU)};
            const DBU eolWidth{std::lround(layer->spacingEolWidth(iSpace) * libDBU)};
            const DBU eolWithin{std::lround(layer->spacingEolWithin(iSpace) * libDBU)};
            const DBU parSpace{std::lround(layer->spacingParSpace(iSpace) * libDBU)};
            const DBU parWithin{std::lround(layer->spacingParWithin(iSpace) * libDBU)};
            if (layer->hasSpacingParellelEdge(iSpace)) {
                spaceRules.emplace_back(space, eolWidth, eolWithin, parSpace, parWithin);
                maxEolSpace = std::max(maxEolSpace, space);
                maxEolWidth = std::max(maxEolWidth, eolWidth);
                maxEolWithin = std::max(maxEolWithin, eolWithin);
            } else if (layer->hasSpacingEndOfLine(iSpace)) {
                spaceRules.emplace_back(space, eolWidth, eolWithin);
                maxEolSpace = std::max(maxEolSpace, space);
                maxEolWidth = std::max(maxEolWidth, eolWidth);
                maxEolWithin = std::max(maxEolWithin, eolWithin);
            } else if (!numSpaceTable) {
                parallelWidthSpace[0][0] = space;
                defaultSpace = getParaRunSpace(width);
            } else if (space != defaultSpace) {
                log() << "Warning in " << __func__ << ": For " << name
                      << ", mismatched defaultSpace & spacingTable...\n";
            }
        }
        if (spaceRules.empty()) {
            log() << "Warning in " << __func__ << ": For " << name << ", no eol spacing rules..." << std::endl;
        }
    }

    for (unsigned iProp = 0; static_cast<int>(iProp) < layer->numProps(); ++iProp) {
        if (!strcmp(layer->propName(iProp), "LEF58_CORNERSPACING")) {
            //  corner spacing
            if (hasCornerSpace()) {
                log() << "Warning in " << __func__ << ": For " << name
                      << ", multiple corner spacing rules: " << layer->propValue(iProp) << "...\n";
                continue;
            }

            std::istringstream iss(layer->propValue(iProp));
            std::string sBuf("");
            double fBuf1{0};
            double fBuf2{0};
            while (iss) {
                iss >> sBuf;
                if (sBuf == "CORNERSPACING" || sBuf == "CONVEXCORNER" || sBuf == ";") {
                    continue;
                }

                if (sBuf == "EXCEPTEOL") {
                    cornerExceptEol = true;
                    iss >> fBuf1;
                    cornerEolWidth = std::lround(fBuf1 * libDBU);
                } else if (sBuf == "WIDTH") {
                    iss >> fBuf1 >> sBuf >> fBuf2;
                    if (fBuf1) {
                        cornerWidth.push_back(std::lround(fBuf1 * libDBU));
                        cornerWidthSpace.push_back(std::lround(fBuf2 * libDBU));
                    } else {
                        cornerWidthSpace[0] = std::lround(fBuf2 * libDBU);
                    }
                } else {
                    log() << "Warning in " << __func__ << ": For " << name
                          << ", corner spacing not identified: " << sBuf << "...\n";
                }
            }
        } else if (!strcmp(layer->propName(iProp), "LEF57_SPACING")) {
            //  eol spacing
            std::istringstream iss(layer->propValue(iProp));
            std::string sBuf("");
            double space{0};
            double eolWidth{0};
            double eolWithin{0};
            double parSpace{0};
            double parWithin{0};
            bool hasEol{false};
            bool hasPar{false};
            while (iss) {
                iss >> sBuf;
                if (sBuf == ";") {
                    continue;
                }

                if (sBuf == "SPACING") {
                    iss >> space;
                } else if (sBuf == "ENDOFLINE") {
                    iss >> eolWidth >> sBuf >> eolWithin;
                    hasEol = true;
                } else if (sBuf == "PARALLELEDGE") {
                    iss >> parSpace >> sBuf >> parWithin;
                    hasPar = true;
                } else {
                    log() << "Warning in " << __func__ << ": For " << name << ", eol spacing not identified: " << sBuf
                          << "...\n";
                }
            }
            if (hasPar) {
                spaceRules.emplace_back(std::lround(space * libDBU),
                                        std::lround(eolWidth),
                                        std::lround(eolWithin),
                                        std::lround(parSpace),
                                        std::lround(parWithin));
                maxEolSpace = std::max(maxEolSpace, std::llround(space * libDBU));
                maxEolWidth = std::max(maxEolWidth, std::llround(eolWidth * libDBU));
                maxEolWithin = std::max(maxEolWithin, std::llround(eolWithin * libDBU));
            } else if (hasEol) {
                spaceRules.emplace_back(std::lround(space * libDBU), std::lround(eolWidth), std::lround(eolWithin));
                maxEolSpace = std::max(maxEolSpace, std::llround(space * libDBU));
                maxEolWidth = std::max(maxEolWidth, std::llround(eolWidth * libDBU));
                maxEolWithin = std::max(maxEolWithin, std::llround(eolWithin * libDBU));
            } else if (!numSpaceTable) {
                parallelWidthSpace[0][0] = std::lround(space * libDBU);
                defaultSpace = getParaRunSpace(width);
            } else if (std::lround(space * libDBU) != defaultSpace) {
                log() << "Warning in " << __func__ << ": For " << name
                      << ", mismatched defaultSpace & spacingTable...\n";
            }
        } else {
            log() << "Warning in " << __func__ << ": For " << name << ", unknown prop: " << layer->propName(iProp)
                  << "...\n";
        }
    }
    fixedMetalQueryMargin = std::max(maxEolSpace, maxEolWithin);

    // Rsyn::PhysicalTracks (DEF)
    // note: crossPoints will be initialized in LayerList
    for (const Rsyn::PhysicalTracks& rsynTrack : rsynTracks) {
        if ((rsynTrack.getDirection() == Rsyn::TRACK_HORIZONTAL) == (direction == X)) {
            continue;
        }
        pitch = rsynTrack.getSpace();
        DBU location = rsynTrack.getLocation();
        for (int i = 0; i < rsynTrack.getNumberOfTracks(); ++i) {
            tracks.emplace_back(location);
            location += pitch;
        }
    }
    delete rsynLayer.getLayer();
    sort(tracks.begin(), tracks.end(), [](const Track& lhs, const Track& rhs) { return lhs.location < rhs.location; });
    pitch = tracks[1].location - tracks[0].location;

    // safe margin
    minAreaMargin = ceil(((minArea / width) + width) * 1.0 / pitch) * pitch * 2;

    // Check consistency between LEF and DEF
    check();
}

bool MetalLayer::isTrackRangeValid(const utils::IntervalT<int>& trackRange) const {
    return trackRange.low >= 0 && trackRange.high < tracks.size() && trackRange.IsValid();
}

bool MetalLayer::isTrackRangeWeaklyValid(const utils::IntervalT<int>& trackRange) const {
    return trackRange.low >= 0 && trackRange.low < tracks.size() && trackRange.high >= 0 &&
           trackRange.high < tracks.size();
}

utils::IntervalT<int> MetalLayer::getUpperCrossPointRange(const utils::IntervalT<int>& trackRange) const {
    return {tracks[trackRange.low].upperCPIdx, tracks[trackRange.high].upperCPIdx};
}

utils::IntervalT<int> MetalLayer::getLowerCrossPointRange(const utils::IntervalT<int>& trackRange) const {
    return {tracks[trackRange.low].lowerCPIdx, tracks[trackRange.high].lowerCPIdx};
}

utils::IntervalT<int> MetalLayer::getSurroundingTrack(DBU loc) const {
    // offset by firstTrackLoc(), make it within track range, and round
    const double floatingTrackIdx =
        (std::min(std::max(firstTrackLoc(), loc), lastTrackLoc()) - firstTrackLoc()) / static_cast<double>(pitch);
    return {floor(floatingTrackIdx), ceil(floatingTrackIdx)};
}

utils::IntervalT<int> MetalLayer::rangeSearchTrack(const utils::IntervalT<DBU>& locRange, bool includeBound) const {
    auto locRangeCopy = locRange;
    // invalid range (low >= high) will still be invalid
    if (locRangeCopy.low < firstTrackLoc()) {
        locRangeCopy.low = firstTrackLoc();
    }
    if (locRangeCopy.high > lastTrackLoc()) {
        locRangeCopy.high = lastTrackLoc();
    }

    utils::IntervalT<int> res{ceil(double(locRangeCopy.low - firstTrackLoc()) / double(pitch)),
                              floor(double(locRangeCopy.high - firstTrackLoc()) / double(pitch))};

    if (!includeBound) {
        if (res.high >= 0 && res.high < numTracks() && tracks[res.high].location == locRange.high) --res.high;
        if (res.low >= 0 && res.low < numTracks() && tracks[res.low].location == locRange.low) ++res.low;
    }

    return res;
}

bool MetalLayer::isCrossPointRangeValid(const utils::IntervalT<int>& crossPointRange) const {
    return crossPointRange.low >= 0 && crossPointRange.high < crossPoints.size() && crossPointRange.IsValid();
}

void MetalLayer::initAccCrossPointDistCost() {
    accCrossPointDistCost.resize(numCrossPoints() + 1);
    accCrossPointDistCost[0] = 0;
    // For four uniform-distributed crossPoints with dist 1:
    // accCrossPointCost = {0, 0.5, 1.5, 2.5, 3}
    for (int cpIdx = 0; cpIdx < numCrossPoints(); ++cpIdx) {
        DBU delta = 0;
        if ((cpIdx + 1) < numCrossPoints()) {
            delta = (crossPoints[cpIdx + 1].location - crossPoints[cpIdx].location) / 2;
        }
        accCrossPointDistCost[cpIdx + 1] = crossPoints[cpIdx].location - crossPoints[0].location + delta;
    }
}

DBU MetalLayer::getCrossPointRangeDistCost(const utils::IntervalT<int>& crossPointRange) const {
    assert(crossPointRange.IsValid());
    return accCrossPointDistCost[crossPointRange.high + 1] - accCrossPointDistCost[crossPointRange.low];
}

DBU MetalLayer::getCrossPointRangeDist(const utils::IntervalT<int>& crossPointRange) const {
    return crossPoints[crossPointRange.high].location - crossPoints[crossPointRange.low].location;
}

utils::PointT<DBU> MetalLayer::getLoc(const GridPoint& grid) const {
    utils::PointT<DBU> loc;
    loc[direction] = tracks[grid.trackIdx].location;
    loc[1 - direction] = crossPoints[grid.crossPointIdx].location;
    return loc;
}

std::pair<utils::PointT<DBU>, utils::PointT<DBU>> MetalLayer::getLoc(const GridEdge& edge) const {
    utils::PointT<DBU> loc1 = getLoc(edge.u);
    utils::PointT<DBU> loc2 = getLoc(edge.v);

    if (loc1.x == loc2.x) {
        if (loc1.y < loc2.y)
            return {loc1, loc2};
        else
            return {loc2, loc1};
    } else {
        if (loc1.x < loc2.x)
            return {loc1, loc2};
        else
            return {loc2, loc1};
    }
}

BoxOnLayer MetalLayer::getLoc(const GridBoxOnLayer& gridBox) const {
    BoxOnLayer box;
    box.layerIdx = gridBox.layerIdx;
    box[direction].Set(tracks[gridBox.trackRange.low].location, tracks[gridBox.trackRange.high].location);
    box[1 - direction].Set(crossPoints[gridBox.crossPointRange.low].location,
                           crossPoints[gridBox.crossPointRange.high].location);
    return box;
}

GridPoint MetalLayer::getUpper(const GridPoint& cur) const {
    return {cur.layerIdx + 1, crossPoints[cur.crossPointIdx].upperTrackIdx, tracks[cur.trackIdx].upperCPIdx};
}

GridPoint MetalLayer::getLower(const GridPoint& cur) const {
    return {cur.layerIdx - 1, crossPoints[cur.crossPointIdx].lowerTrackIdx, tracks[cur.trackIdx].lowerCPIdx};
}

bool MetalLayer::isValid(const GridPoint& gridPt) const {
    return gridPt.trackIdx >= 0 && gridPt.trackIdx < tracks.size() &&               // track
           gridPt.crossPointIdx >= 0 && gridPt.crossPointIdx < crossPoints.size();  // cross point
}

bool MetalLayer::isValid(const GridBoxOnLayer& gridBox) const {
    return isTrackRangeValid(gridBox.trackRange) && isCrossPointRangeValid(gridBox.crossPointRange);
}

DBU MetalLayer::getParaRunSpace(const DBU width, const DBU length) const {
    int iWidth = parallelWidth.size() - 1;  // first smaller than or equal to
    while (iWidth > 0 && parallelWidth[iWidth] >= width) {
        --iWidth;
    }
    if (length == 0) return parallelWidthSpace[iWidth][0];  // fast return
    int iLength = parallelLength.size() - 1;                // first smaller than or equal to
    while (iLength > 0 && parallelLength[iLength] >= length) {
        --iLength;
    }
    return parallelWidthSpace[iWidth][iLength];
}

DBU MetalLayer::getParaRunSpace(const utils::BoxT<DBU>& targetMetal, const DBU length) const {
    return getParaRunSpace(min(targetMetal.width(), targetMetal.height()), length);
}

DBU MetalLayer::getSpace(const utils::BoxT<DBU>& targetMetal, int dir, AggrParaRunSpace aggr) const {
    const DBU range = targetMetal[1 - dir].range();
    DBU space = getEolSpace(range);
    if (!space) {
        // parallel run spacing
        DBU length = 0;
        if (aggr == AggrParaRunSpace::LARGER_LENGTH && targetMetal[1 - dir].range() > 100 * pitch) {
            // hack: assume at least two-pitch parallel run length
            length = (pitch * 2 + width);
        }
        space = getParaRunSpace(targetMetal, length);
    }
    // do not know the width of neighbor, so paraRunSpaceForLargerWidth
    if (aggr == AggrParaRunSpace::LARGER_WIDTH && paraRunSpaceForLargerWidth > space) {
        space = paraRunSpaceForLargerWidth;
    }
    return space;
}

DBU MetalLayer::getEolSpace(const DBU width) const { return (width < maxEolWidth) ? maxEolSpace : 0; }

bool MetalLayer::isEolViolation(const DBU space, const DBU width, const DBU within) const {
    return (space < maxEolSpace && width < maxEolWidth && within < maxEolWithin);
}

bool MetalLayer::isEolViolation(const utils::BoxT<DBU>& lhs, const utils::BoxT<DBU>& rhs) const {
    for (unsigned dim = 0; dim != 2; ++dim) {
        const DBU space = utils::Dist(lhs[1 - dim], rhs[1 - dim]);
        const DBU width = min(lhs[dim].range(), rhs[dim].range());
        const DBU within = utils::Dist(lhs[dim], rhs[dim]);
        if (isEolViolation(space, width, within)) return true;
    }
    return false;
}

DBU MetalLayer::getCornerSpace(const DBU width) const {
    if (cornerExceptEol && width < cornerEolWidth) return 0;

    int iWidth = cornerWidth.size() - 1;  // first smaller than or equal to
    while (iWidth > 0 && cornerWidth[iWidth] >= width) {
        --iWidth;
    }
    return cornerWidthSpace[iWidth];
}

DBU MetalLayer::getCornerSpace(const utils::BoxT<DBU>& targetMetal) const {
    return getCornerSpace(min(targetMetal.width(), targetMetal.height()));
}

void MetalLayer::initWireRange() {
    const DBU eolSpace = getEolSpace(width);
    const DBU wireEndPointSpace = eolSpace ? eolSpace + width : defaultSpace + width;  // consider two half width
    wireRange.resize(numCrossPoints(), {0, 0});
    int i, j;
    for (int cpIdx = 0; cpIdx < numCrossPoints(); ++cpIdx) {
        i = 0;
        while (cpIdx + i >= 0 && crossPoints[cpIdx].location - crossPoints[cpIdx + i].location < wireEndPointSpace) {
            --i;
        }
        j = 0;
        while (cpIdx + j < numCrossPoints() &&
               crossPoints[cpIdx + j].location - crossPoints[cpIdx].location < wireEndPointSpace) {
            ++j;
        }
        wireRange[cpIdx] = {i + 1, j - 1};
    }
}

ostream& MetalLayer::printBasics(ostream& os) const {
    os << name << ": dir=" << getDimension(direction) << ", idx=" << idx;
    os << ", tracks=(locs=" << firstTrackLoc() << "-" << lastTrackLoc() << ",pitch=" << pitch << ",#=" << numTracks()
       << ")";
    os << ", crossPts=(locs=" << firstCrossPointLoc() << "-" << lastCrossPointLoc() << ",#=" << numCrossPoints() << ")";
    os << ", #grids=" << numGridPoints() << ")";
    return os;
}

ostream& MetalLayer::printDesignRules(ostream& os) const {
    os << name << ": width=" << width << ", paraSpace=(default=" << defaultSpace;
    for (int i = 0; i < parallelWidth.size(); ++i) {
        os << ", " << parallelWidth[i] << ":" << parallelWidthSpace[i];
    }
    os << "), eolSpace=(";
    for (const SpaceRule& spaceRule : spaceRules) {
        if (spaceRule.hasEol /* && !spaceRule.hasPar */) {
            os << spaceRule.eolWidth << ':' << spaceRule.space << ", ";
        }
    }
    os << "), minArea=" << minArea;
    return os;
}

ostream& MetalLayer::printViaOccupancyLUT(ostream& os) const {
    auto getMaxSize2d = [](const vector<vector<vector<bool>>>& LUT, size_t& xSize, size_t& ySize) {
        xSize = 0;
        ySize = 0;
        for (const vector<vector<bool>>& cpLUT : LUT) {
            if (cpLUT.size()) {
                xSize = max(xSize, cpLUT.size());
                ySize = max(ySize, cpLUT[0].size());
            }
        }
    };
    auto getMaxSize1d = [](const vector<utils::IntervalT<int>>& ranges, utils::IntervalT<int>& yRange) {
        yRange = {0, 0};
        for (const auto& rangeCP : ranges) {
            yRange = yRange.UnionWith(rangeCP);
        }
    };
    size_t xSize, ySize;
    utils::IntervalT<int> yRange;
    getMaxSize1d(wireRange, yRange);
    os << name << ": wire(" << wireRange.size() << ',' << yRange << ')';
    if (wireBotVia.size()) {
        getMaxSize2d(wireBotVia[0], xSize, ySize);  // TODO: print default
        os << ", wireBotVia(" << wireBotVia.size() << ',' << xSize << ',' << ySize << ')';
    }
    if (wireTopVia.size()) {
        getMaxSize2d(wireTopVia[0], xSize, ySize);
        os << ", wireTopVia(" << wireTopVia.size() << ',' << xSize << ',' << ySize << ')';
    }
    return os;
}

ostream& operator<<(ostream& os, const MetalLayer& layer) { return layer.printBasics(os); }

void MetalLayer::check() const {
    if (width < minWidth) {
        log() << "Warning: In layer " << name << ", width = " << width << " < minWidth = " << minWidth << std::endl;
    }
    if (width > maxEolWidth) {
        log() << "Warning: In layer " << name << ", width = " << width << " > maxEolWidth = " << maxEolWidth
              << std::endl;
    }
    if (width + defaultSpace > pitch) {
        log() << "Warning: In layer " << name << ", width + defaultSpace =" << width + defaultSpace
              << " > picth = " << pitch << std::endl;
    }
}

}  // namespace db
