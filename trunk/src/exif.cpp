// ***************************************************************** -*- C++ -*-
/*
 * Copyright (C) 2004 Andreas Huggel <ahuggel@gmx.net>
 * 
 * This program is part of the Exiv2 distribution.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 * 
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
/*
  File:      exif.cpp
  Version:   $Name:  $ $Revision: 1.21 $
  Author(s): Andreas Huggel (ahu) <ahuggel@gmx.net>
  History:   26-Jan-04, ahu: created
             11-Feb-04, ahu: isolated as a component
 */
// *****************************************************************************
#include "rcsid.hpp"
EXIV2_RCSID("@(#) $Name:  $ $Revision: 1.21 $ $RCSfile: exif.cpp,v $")

// *****************************************************************************
// included header files
#include "exif.hpp"
#include "types.hpp"
#include "error.hpp"
#include "value.hpp"
#include "ifd.hpp"
#include "tags.hpp"
#include "image.hpp"
#include "makernote.hpp"

// + standard includes
#include <iostream>
#include <iomanip>
#include <sstream>
#include <fstream>
#include <utility>
#include <algorithm>
#include <map>
#include <cstring>

// *****************************************************************************
// local declarations
namespace {

    /*
      Set the data of the entry identified by tag in ifd to an unsigned long
      with the value of offset. If no entry with this tag exists in ifd, an
      entry of type unsigned long with one component is created.
     */
    void setOffsetTag(Exif::Ifd& ifd,
                      Exif::uint16 tag,
                      Exif::uint32 offset, 
                      Exif::ByteOrder byteOrder);

}

// *****************************************************************************
// class member definitions
namespace Exif {

    Metadatum::Metadatum(const Entry& e, ByteOrder byteOrder)
        : tag_(e.tag()), ifdId_(e.ifdId()), makerNote_(e.makerNote()),
          value_(0)
    {
        value_ = Value::create(TypeId(e.type()));
        value_->read(e.data(), e.size(), byteOrder);
        key_ = makeKey(tag_, ifdId_, makerNote_);
    }

    Metadatum::Metadatum(const std::string& key, 
                         const Value* value, 
                         MakerNote* makerNote)
        : makerNote_(makerNote), value_(0), key_(key)
    {
        if (value) value_ = value->clone();
        std::pair<uint16, IfdId> p = decomposeKey(key, makerNote);
        if (p.first == 0xffff) throw Error("Invalid key");
        tag_ = p.first;
        if (p.second == ifdIdNotSet) throw Error("Invalid key");
        ifdId_ = p.second;
    }

    Metadatum::~Metadatum()
    {
        delete value_;
        // do *not* delete the MakerNote
    }

    Metadatum::Metadatum(const Metadatum& rhs)
        : tag_(rhs.tag_), ifdId_(rhs.ifdId_), makerNote_(rhs.makerNote_),
          value_(0), key_(rhs.key_)
    {
        if (rhs.value_ != 0) value_ = rhs.value_->clone(); // deep copy
    }

    Metadatum& Metadatum::operator=(const Metadatum& rhs)
    {
        if (this == &rhs) return *this;
        tag_ = rhs.tag_;
        ifdId_ = rhs.ifdId_;
        makerNote_ = rhs.makerNote_;
        delete value_;
        value_ = 0;
        if (rhs.value_ != 0) value_ = rhs.value_->clone(); // deep copy
        key_ = rhs.key_;
        return *this;
    } // Metadatum::operator=
    
    void Metadatum::setValue(const Value* value)
    {
        delete value_;
        value_ = value->clone();
    }

    void Metadatum::setValue(const std::string& buf)
    {
        if (value_ == 0) value_ = Value::create(asciiString);
        value_->read(buf);
    }

    std::string Metadatum::tagName() const
    {
        if (ifdId_ == makerIfd && makerNote_ != 0) {
            return makerNote_->tagName(tag_);
        }
        return ExifTags::tagName(tag_, ifdId_); 
    }

    std::string Metadatum::sectionName() const 
    {
        if (ifdId_ == makerIfd && makerNote_ != 0) {
            return makerNote_->sectionName(tag_);
        }        
        return ExifTags::sectionName(tag_, ifdId_); 
    }

    Thumbnail::Thumbnail()
        : type_(none), size_(0), image_(0), ifd_(ifd1, 0, false)
    {
    }

    Thumbnail::~Thumbnail()
    {
        delete[] image_;
    }

    Thumbnail::Thumbnail(const Thumbnail& rhs)
        : type_(rhs.type_), size_(rhs.size_), image_(0), ifd_(ifd1, 0, false)
    {
        if (rhs.image_ > 0 && rhs.size_ > 0) {
            image_ = new char[rhs.size_];
            memcpy(image_, rhs.image_, rhs.size_);
        }
        if (image_ && type_ == tiff) {
            tiffHeader_.read(image_);
            ifd_.read(image_ + tiffHeader_.offset(),
                      tiffHeader_.byteOrder(), tiffHeader_.offset());
        }
    }

    Thumbnail& Thumbnail::operator=(const Thumbnail& rhs)
    {
        type_ = rhs.type_;
        size_ = rhs.size_;
        delete[] image_;
        image_ = 0;
        if (rhs.image_ > 0 && rhs.size_ > 0) {
            image_ = new char[rhs.size_];
            memcpy(image_, rhs.image_, rhs.size_);
        }
        if (image_ && type_ == tiff) {
            tiffHeader_.read(image_);
            ifd_.read(image_ + tiffHeader_.offset(), 
                      tiffHeader_.byteOrder(), tiffHeader_.offset());
        }
        return *this;
    }

    int Thumbnail::read(const char* buf,
                        const ExifData& exifData,
                        ByteOrder byteOrder)
    {
        int rc = 0;
        std::string key = "Thumbnail.ImageStructure.Compression";
        ExifData::const_iterator pos = exifData.findKey(key);
        if (pos == exifData.end()) return -1; // no thumbnail
        long compression = pos->toLong();
        if (compression == 6) {
            rc = readJpegImage(buf, exifData);
        }
        else {
            rc = readTiffImage(buf, exifData, byteOrder);
        }
        return rc;
    } // Thumbnail::read

    int Thumbnail::readJpegImage(const char* buf, const ExifData& exifData) 
    {
        std::string key = "Thumbnail.RecordingOffset.JPEGInterchangeFormat";
        ExifData::const_iterator pos = exifData.findKey(key);
        if (pos == exifData.end()) return 1;
        long offset = pos->toLong();
        key = "Thumbnail.RecordingOffset.JPEGInterchangeFormatLength";
        pos = exifData.findKey(key);
        if (pos == exifData.end()) return 1;
        long size = pos->toLong();
        image_ = new char[size];
        memcpy(image_, buf + offset, size);
        size_ = size;
        type_ = jpeg;
        return 0;
    } // Thumbnail::readJpegImage

    int Thumbnail::readTiffImage(const char* buf,
                                 const ExifData& exifData,
                                 ByteOrder byteOrder)
    {
        char* data = new char[64*1024];     // temporary buffer Todo: handle larger
        memset(data, 0x0, 64*1024);         // images (which violate the Exif Std)
        long len = 0;                       // number of bytes in the buffer

        // Copy the TIFF header
        TiffHeader tiffHeader(byteOrder);
        len += tiffHeader.copy(data);

        // Create IFD (without Exif and GPS tags) from metadata
        Ifd ifd1(ifd1);
        addToIfd(ifd1, exifData.begin(), exifData.end(), tiffHeader.byteOrder());
        Ifd::iterator i = ifd1.findTag(0x8769);
        if (i != ifd1.end()) ifd1.erase(i);
        i = ifd1.findTag(0x8825);
        if (i != ifd1.end()) ifd1.erase(i);

        // Do not copy the IFD yet, remember the location and leave a gap
        long ifdOffset = len;
        len += ifd1.size() + ifd1.dataSize();

        // Copy thumbnail image data, remember the offsets used
        std::string key = "Thumbnail.RecordingOffset.StripOffsets";
        ExifData::const_iterator offsets = exifData.findKey(key);
        if (offsets == exifData.end()) return 2;
        key = "Thumbnail.RecordingOffset.StripByteCounts";
        ExifData::const_iterator sizes = exifData.findKey(key);
        if (sizes == exifData.end()) return 2;
        std::ostringstream os;                  // for the new strip offsets
        for (long k = 0; k < offsets->count(); ++k) {
            long offset = offsets->toLong(k);
            long size = sizes->toLong(k);
            memcpy(data + len, buf + offset, size);
            os << len << " ";
            len += size;
        }

        // Update the IFD with the actual strip offsets (replace existing entry)
        Metadatum newOffsets(*offsets);
        newOffsets.setValue(os.str());
        addToIfd(ifd1, newOffsets, tiffHeader.byteOrder());

        // Finally, sort and copy the IFD
        ifd1.sortByTag();
        ifd1.copy(data + ifdOffset, tiffHeader.byteOrder(), ifdOffset);

        image_ = new char[len];
        memcpy(image_, data, len);
        size_ = len;
        tiffHeader_.read(image_);
        ifd_.read(image_ + tiffHeader_.offset(), 
                  tiffHeader_.byteOrder(), tiffHeader_.offset());
        type_ = tiff;
        delete[] data;

        return 0;
    } // Thumbnail::readTiffImage

    int Thumbnail::write(const std::string& path) const
    {
        std::string p;
        switch (type_) {
        case jpeg: 
            p = path + ".jpg";
            break;
        case tiff:
            p = path + ".tif";
            break;
        case none:
            return 1;
            break;
        }
        std::ofstream file(p.c_str(), std::ios::binary);
        if (!file) return 1;
        file.write(image_, size_);
        if (!file.good()) return 2;
        return 0;
    } // Thumbnail::write

    void Thumbnail::update(ExifData& exifData) const
    {
        // Todo: properly synchronize the Exif data with the actual thumbnail,
        //       i.e., synch all relevant metadata

        switch (type_) {
        case jpeg: 
            updateJpegImage(exifData);
            break;
        case tiff:
            updateTiffImage(exifData);
            break;
        case none:
            /* do nothing */
            break;
        }

    } // Thumbnail::update

    void Thumbnail::updateJpegImage(ExifData& exifData) const
    {
        std::string key = "Thumbnail.RecordingOffset.JPEGInterchangeFormat";
        ExifData::iterator pos = exifData.findKey(key);
        if (pos == exifData.end()) {
            Value *value = Value::create(unsignedLong);
            exifData.add(key, value);
            delete value;
            pos = exifData.findKey(key);
        }
        pos->setValue("0");

        key = "Thumbnail.RecordingOffset.JPEGInterchangeFormatLength";
        pos = exifData.findKey(key);
        if (pos == exifData.end()) {
            Value *value = Value::create(unsignedLong);
            exifData.add(key, value);
            delete value;            
            pos = exifData.findKey(key);
        }
        pos->setValue(toString(size_));

    } // Thumbnail::updateJpegImage

    void Thumbnail::updateTiffImage(ExifData& exifData) const
    {
        // Create metadata from the StripOffsets and StripByteCounts entries
        // and add these to the Exif data, replacing existing entries
        Ifd::const_iterator pos = ifd_.findTag(0x0111);
        if (pos == ifd_.end()) throw Error("Bad thumbnail (0x0111)");
        exifData.add(Metadatum(*pos, tiffHeader_.byteOrder()));

        pos = ifd_.findTag(0x0117);
        if (pos == ifd_.end()) throw Error("Bad thumbnail (0x0117)");
        exifData.add(Metadatum(*pos, tiffHeader_.byteOrder()));

    } // Thumbnail::updateTiffImage

    long Thumbnail::copy(char* buf) const
    {
        long ret = 0;
        switch (type_) {
        case jpeg: 
            ret = copyJpegImage(buf);
            break;
        case tiff:
            ret = copyTiffImage(buf);
            break;
        case none:
            ret = 0;
            break;
        }
        return ret;
    }

    long Thumbnail::copyJpegImage(char* buf) const
    {
        memcpy(buf, image_, size_);
        return size_;
    }

    long Thumbnail::copyTiffImage(char* buf) const
    {
        long offset = ifd_.offset() + ifd_.size() + ifd_.dataSize();
        long size = size_ - offset;
        memcpy(buf, image_ + offset, size);
        return size;
    }

    long Thumbnail::size() const
    {
        long size = 0;
        switch (type_) {
        case jpeg: 
            size = size_;
            break;
        case tiff:
            size = size_ - ifd_.offset() - ifd_.size() - ifd_.dataSize();
            break;
        case none:
            size = 0;
            break;
        }
        return size;
    }

    void Thumbnail::setOffsets(Ifd& ifd1, ByteOrder byteOrder) const
    {
        switch (type_) {
        case jpeg: 
            setJpegImageOffsets(ifd1, byteOrder);
            break;
        case tiff:
            setTiffImageOffsets(ifd1, byteOrder);
            break;
        case none:
            /* do nothing */
            break;
        }        
    }

    void Thumbnail::setJpegImageOffsets(Ifd& ifd1, ByteOrder byteOrder) const
    {
        Ifd::iterator pos = ifd1.findTag(0x0201);
        if (pos == ifd1.end()) throw Error("Bad thumbnail (0x0201)");
        pos->setValue(ifd1.offset() + ifd1.size() + ifd1.dataSize(), byteOrder);
    }

    void Thumbnail::setTiffImageOffsets(Ifd& ifd1, ByteOrder byteOrder) const
    {
        // Adjust the StripOffsets, assuming that the existing TIFF strips
        // start immediately after the thumbnail IFD
        long shift = ifd1.offset() + ifd1.size() + ifd1.dataSize() 
            - ifd_.offset() - ifd_.size() - ifd_.dataSize();
        Ifd::const_iterator pos = ifd_.findTag(0x0111);
        if (pos == ifd_.end()) throw Error("Bad thumbnail (0x0111)");
        Metadatum offsets(*pos, tiffHeader_.byteOrder());
        std::ostringstream os;
        for (long k = 0; k < offsets.count(); ++k) {
            os << offsets.toLong(k) + shift << " ";
        }
        offsets.setValue(os.str());

        // Write the offsets to IFD1, encoded in the corresponding byte order
        addToIfd(ifd1, offsets, byteOrder);

    } // Thumbnail::setTiffImageOffsets

    ExifData::ExifData() 
        : makerNote_(0), ifd0_(ifd0, 0, false), exifIfd_(exifIfd, 0, false), 
          iopIfd_(iopIfd, 0, false), gpsIfd_(gpsIfd, 0, false),
          ifd1_(ifd1, 0, false), valid_(false), size_(0), data_(0)
    {
    }

    ExifData::~ExifData()
    {
        delete makerNote_;
        delete[] data_;
    }

    int ExifData::read(const std::string& path)
    {
        JpegImage img;
        int rc = img.readExifData(path);
        if (rc) return rc;
        return read(img.exifData(), img.sizeExifData());
    }

    int ExifData::read(const char* buf, long len)
    {
        // Copy the data buffer
        delete[] data_;
        data_ = new char[len];
        memcpy(data_, buf, len);
        size_ = len;
        valid_ = true;

        // Read the TIFF header
        int ret = 0;
        int rc = tiffHeader_.read(data_);
        if (rc) return rc;

        // Read IFD0
        rc = ifd0_.read(data_ + tiffHeader_.offset(), 
                        byteOrder(), 
                        tiffHeader_.offset());
        if (rc) return rc;
        // Find and read ExifIFD sub-IFD of IFD0
        rc = ifd0_.readSubIfd(exifIfd_, data_, byteOrder(), 0x8769);
        if (rc) return rc;
        // Find and read Interoperability IFD in ExifIFD
        rc = exifIfd_.readSubIfd(iopIfd_, data_, byteOrder(), 0xa005);
        if (rc) return rc;
        // Find and read GPSInfo sub-IFD in IFD0
        rc = ifd0_.readSubIfd(gpsIfd_, data_, byteOrder(), 0x8825);
        if (rc) return rc;
        // Read IFD1
        if (ifd0_.next()) {
            rc = ifd1_.read(data_ + ifd0_.next(), byteOrder(), ifd0_.next());
            if (rc) return rc;
        }

        // Find MakerNote in ExifIFD, create a MakerNote class 
        Ifd::iterator pos = exifIfd_.findTag(0x927c);
        Ifd::iterator make = ifd0_.findTag(0x010f);
        Ifd::iterator model = ifd0_.findTag(0x0110);
        if (pos != exifIfd_.end() && make != ifd0_.end() && model != ifd0_.end()) {
            MakerNoteFactory& makerNoteFactory = MakerNoteFactory::instance();
            // Todo: The conversion to string assumes that there is a \0 at the end
            makerNote_ = makerNoteFactory.create(make->data(), model->data());
        }
        // Read the MakerNote
        if (makerNote_) {
            rc = makerNote_->read(pos->data(), 
                                  pos->size(),
                                  byteOrder(),
                                  exifIfd_.offset() + pos->offset());
            if (rc) {
                delete makerNote_;
                makerNote_ = 0;
            }
        }
        // If we successfully parsed the MakerNote, delete the raw MakerNote,
        // the parsed MakerNote is the primary MakerNote from now on
        if (makerNote_) {
            exifIfd_.erase(pos);
        }

        // Find and delete ExifIFD sub-IFD of IFD1
        pos = ifd1_.findTag(0x8769);
        if (pos != ifd1_.end()) {
            ifd1_.erase(pos);
            ret = -99;
        }
        // Find and delete GPSInfo sub-IFD in IFD1
        pos = ifd1_.findTag(0x8825);
        if (pos != ifd1_.end()) {
            ifd1_.erase(pos);
            ret = -99;
        }

        // Copy all entries from the IFDs and the MakerNote to the metadata
        metadata_.clear();
        add(ifd0_.begin(), ifd0_.end(), byteOrder());
        add(exifIfd_.begin(), exifIfd_.end(), byteOrder());
        if (makerNote_) {
            add(makerNote_->begin(), makerNote_->end(), byteOrder());
        }
        add(iopIfd_.begin(), iopIfd_.end(), byteOrder()); 
        add(gpsIfd_.begin(), gpsIfd_.end(), byteOrder());
        add(ifd1_.begin(), ifd1_.end(), byteOrder());

        // Read the thumbnail
        thumbnail_.read(data_, *this, byteOrder());

        return ret;
    } // ExifData::read

    int ExifData::write(const std::string& path) 
    {
        long size = this->size();
        char* buf = new char[size];
        long actualSize = copy(buf);
        if (actualSize > size) {
            throw Error("Invariant violated in ExifData::write");
        }
        JpegImage img;
        img.setExifData(buf, actualSize);
        delete[] buf;
        return img.writeExifData(path);
    } // ExifData::write

    long ExifData::copy(char* buf)
    {
        long size = 0;
        // If we can update the internal IFDs and the underlying data buffer
        // from the metadata without changing the data size, then it is enough
        // to copy the data buffer.
        if (updateIfds()) {

//ahu Todo: remove debugging output
std::cout << "->>>>>> using non-intrusive writing <<<<<<-\n";

            memcpy(buf, data_, size_);
            size = size_;
        }
        // Else we have to do it the hard way...
        else {

//ahu Todo: remove debugging output
std::cout << "->>>>>> writing from metadata <<<<<<-\n";

            size = copyFromMetadata(buf);
        }
        return size;
    }

    long ExifData::copyFromMetadata(char* buf)
    {
        // Copy the TIFF header
        long ifd0Offset = tiffHeader_.copy(buf);

        // Build IFD0
        Ifd ifd0(ifd0, ifd0Offset);
        addToIfd(ifd0, begin(), end(), byteOrder());

        // Build Exif IFD from metadata
        long exifIfdOffset = ifd0Offset + ifd0.size() + ifd0.dataSize();
        Ifd exifIfd(exifIfd, exifIfdOffset);
        addToIfd(exifIfd, begin(), end(), byteOrder());

        // Set the offset to the Exif IFD in IFD0
        ifd0.erase(0x8769);
        if (exifIfd.size() > 0) {
            setOffsetTag(ifd0, 0x8769, exifIfdOffset, byteOrder());
        }

        // Build Interoperability IFD from metadata
        long iopIfdOffset = exifIfdOffset + exifIfd.size() + exifIfd.dataSize();
        Ifd iopIfd(iopIfd, iopIfdOffset);
        addToIfd(iopIfd, begin(), end(), byteOrder());

        // Set the offset to the Interoperability IFD in Exif IFD
        exifIfd.erase(0xa005);
        if (iopIfd.size() > 0) {
            setOffsetTag(exifIfd, 0xa005, iopIfdOffset, byteOrder());
        }

        // Build GPSInfo IFD from metadata
        long gpsIfdOffset = iopIfdOffset + iopIfd.size() + iopIfd.dataSize();
        Ifd gpsIfd(gpsIfd, gpsIfdOffset);
        addToIfd(gpsIfd, begin(), end(), byteOrder());

        // Set the offset to the GPSInfo IFD in IFD0
        ifd0.erase(0x8825);
        if (gpsIfd.size() > 0) {
            setOffsetTag(ifd0, 0x8825, gpsIfdOffset, byteOrder());
        }

        // Update Exif data from thumbnail, build IFD1 from updated metadata
        thumbnail_.update(*this);
        long ifd1Offset = gpsIfdOffset + gpsIfd.size() + gpsIfd.dataSize();
        Ifd ifd1(ifd1, ifd1Offset);
        addToIfd(ifd1, begin(), end(), byteOrder());
        thumbnail_.setOffsets(ifd1, byteOrder());
        long thumbOffset = ifd1Offset + ifd1.size() + ifd1.dataSize();

        // Set the offset to IFD1 in IFD0
        if (ifd1.size() > 0) {
            ifd0.setNext(ifd1Offset);
        }

        // Copy all IFDs and the thumbnail image to the data buffer
        ifd0.sortByTag();
        ifd0.copy(buf + ifd0Offset, byteOrder(), ifd0Offset);
        exifIfd.sortByTag();
        exifIfd.copy(buf + exifIfdOffset, byteOrder(), exifIfdOffset);
        iopIfd.sortByTag();
        iopIfd.copy(buf + iopIfdOffset, byteOrder(), iopIfdOffset);
        gpsIfd.sortByTag();
        gpsIfd.copy(buf + gpsIfdOffset, byteOrder(), gpsIfdOffset);
        ifd1.sortByTag();
        ifd1.copy(buf + ifd1Offset, byteOrder(), ifd1Offset);
        long len = thumbnail_.copy(buf + thumbOffset);

        return len + thumbOffset;

    } // ExifData::copyFromMetadata

    long ExifData::size() const
    {
        long size;
        if (compatible()) {
            size = size_;
        }
        else {
            size = tiffHeader_.size();
            std::map<IfdId, int> ifdEntries;
            const_iterator mdEnd = this->end();
            for (const_iterator md = begin(); md != mdEnd; ++md) {
                size += md->size();
                ifdEntries[md->ifdId()] += 1;
            }
            std::map<IfdId, int>::const_iterator eEnd = ifdEntries.end();
            std::map<IfdId, int>::const_iterator e;
            for (e = ifdEntries.begin(); e != eEnd; ++e) {
                size += 2 + 12 * e->second + 4;
            }
            size += thumbnail_.size();
            // Add 1k to account for the possibility that Thumbnail::update
            // may add entries to IFD1
            size += 1024;
        }
        return size;
    }

    void ExifData::add(Entries::const_iterator begin, 
                       Entries::const_iterator end,
                       ByteOrder byteOrder)
    {
        Entries::const_iterator i = begin;
        for (; i != end; ++i) {
            add(Metadatum(*i, byteOrder));
        }
    }

    void ExifData::add(const std::string& key, Value* value)
    {
        add(Metadatum(key, value));
    }

    void ExifData::add(const Metadatum& metadatum)
    {
        // allow duplicates
        metadata_.push_back(metadatum);
    }

    ExifData::const_iterator ExifData::findKey(const std::string& key) const
    {
        return std::find_if(metadata_.begin(), metadata_.end(),
                            FindMetadatumByKey(key));
    }

    ExifData::iterator ExifData::findKey(const std::string& key)
    {
        return std::find_if(metadata_.begin(), metadata_.end(),
                            FindMetadatumByKey(key));
    }

    void ExifData::sortByKey()
    {
        std::sort(metadata_.begin(), metadata_.end(), cmpMetadataByKey);
    }

    void ExifData::sortByTag()
    {
        std::sort(metadata_.begin(), metadata_.end(), cmpMetadataByTag);
    }

    void ExifData::erase(ExifData::iterator pos)
    {
        metadata_.erase(pos);
    }

    bool ExifData::updateIfds()
    {
        if (!this->compatible()) return false;

        bool compatible = true;
        compatible |= updateIfd(ifd0_);
        compatible |= updateIfd(exifIfd_);
        compatible |= updateIfd(iopIfd_);
        compatible |= updateIfd(gpsIfd_);
        compatible |= updateIfd(ifd1_);
        
        return compatible;
    } // ExifData::updateIfds

    bool ExifData::updateIfd(Ifd& ifd)
    {
        if (ifd.alloc()) throw Error("Invariant violated in ExifData::updateIfd");

        bool compatible = true;
        Ifd::iterator end = ifd.end();
        for (Ifd::iterator entry = ifd.begin(); entry != end; ++entry) {
            // find the corresponding metadatum
            std::string key = 
                makeKey(entry->tag(), entry->ifdId(), entry->makerNote());
            const_iterator md = findKey(key);
            if (md == this->end()) {
                // corresponding metadatum was deleted: this is not (yet) a
                // supported non-intrusive write operation.
                compatible = false;
                continue;
            }
            char* buf = new char[md->size()];
            md->copy(buf, byteOrder());
            entry->setValue(md->typeId(), md->count(), buf, md->size());
            delete[] buf;
        }
        return compatible;
    } // ExifData::updateIfd

    bool ExifData::compatible() const
    {
        bool compatible = true;
        const_iterator end = this->end();
        for (const_iterator md = begin(); md != end; ++md) {
            // Check if the metadatum is compatible with the 
            // corresponding IFD entry
            const Ifd* ifd = getIfd(md->ifdId());
            if (!ifd) {
                compatible = false;
                break;
            }            
            Ifd::const_iterator entry = ifd->findTag(md->tag());
            if (entry == ifd->end()) {
                compatible = false;
                break;
            }
            if (md->size() > entry->size()) {
                compatible = false;
                break;
            }
        }
        return compatible;
    }

    const Ifd* ExifData::getIfd(IfdId ifdId) const
    {
        const Ifd* ifd = 0;
        switch (ifdId) {
        case ifd0: 
            ifd = &ifd0_;
            break;
        case exifIfd: 
            ifd = &exifIfd_;
            break;
        case iopIfd: 
            ifd = &iopIfd_;
            break;
        case gpsIfd: 
            ifd = &gpsIfd_;
            break;
        case ifd1: 
            ifd = &ifd1_;
            break;
        default:
            ifd = 0;
            break;
        }
        return ifd;
    }

    // *************************************************************************
    // free functions

    void addToIfd(Ifd& ifd, 
                  Metadata::const_iterator begin, 
                  Metadata::const_iterator end, 
                  ByteOrder byteOrder)
    {
        for (Metadata::const_iterator i = begin; i != end; ++i) {
            // add only metadata with matching IFD id
            if (i->ifdId() == ifd.ifdId()) {
                addToIfd(ifd, *i, byteOrder);
            }
        }
    } // addToIfd

    void addToIfd(Ifd& ifd, const Metadatum& metadatum, ByteOrder byteOrder)
    {
        // Todo: Implement Assert (Stroustup 24.3.7.2)
        if (!ifd.alloc()) throw Error("Invariant violated in addToIfd");

        Entry e(ifd.alloc());
        e.setIfdId(metadatum.ifdId());
        e.setTag(metadatum.tag());
        e.setOffset(0);  // will be calculated when the IFD is written
        char* buf = new char[metadatum.size()];
        metadatum.copy(buf, byteOrder);
        e.setValue(metadatum.typeId(), metadatum.count(), buf, metadatum.size()); 
        ifd.add(e);
        delete[] buf;
    } // addToIfd

    bool cmpMetadataByTag(const Metadatum& lhs, const Metadatum& rhs)
    {
        return lhs.tag() < rhs.tag();
    }

    bool cmpMetadataByKey(const Metadatum& lhs, const Metadatum& rhs)
    {
        return lhs.key() < rhs.key();
    }

    std::ostream& operator<<(std::ostream& os, const Metadatum& md)
    {
        if (md.ifdId() == makerIfd && md.makerNote() != 0) {
            return md.makerNote()->printTag(os, md.tag(), md.value());
        }
        return ExifTags::printTag(os, md.tag(), md.ifdId(), md.value());
    }

    std::string makeKey(uint16 tag, IfdId ifdId, const MakerNote* makerNote)
    {
        if (ifdId == makerIfd && makerNote != 0) {
            return makerNote->makeKey(tag);
        }
        return ExifTags::makeKey(tag, ifdId);
    }

    std::pair<uint16, IfdId> decomposeKey(const std::string& key,
                                          const MakerNote* makerNote)
    {
        std::pair<uint16, IfdId> p = ExifTags::decomposeKey(key);
        if (p.second == makerIfd && makerNote != 0) {
            p.first = makerNote->decomposeKey(key);
        }
        return p;
    }

}                                       // namespace Exif

// *****************************************************************************
// local definitions
namespace {

    void setOffsetTag(Exif::Ifd& ifd,
                      Exif::uint16 tag,
                      Exif::uint32 offset, 
                      Exif::ByteOrder byteOrder)
    {
        Exif::Ifd::iterator pos = ifd.findTag(tag);
        if (pos == ifd.end()) {
            Exif::Entry e(ifd.alloc());
            e.setIfdId(ifd.ifdId());
            e.setTag(tag);
            e.setOffset(0);  // will be calculated when the IFD is written
            ifd.add(e);
            pos = ifd.findTag(tag);
        }
        pos->setValue(offset, byteOrder);
    }

}
