#include <BidiUtils.h>
#include <Epub/Page.h>
#include <Epub/blocks/TextBlock.h>
#include <Epub/converters/ImageDecoderFactory.h>
#include <Epub/converters/ImageDimsProbe.h>
#include <Epub/hyphenation/Hyphenator.h>
#include <Epub/parsers/PreviewBlockLocator.h>
#include <Epub/tables/CompactTableLayout.h>
#include <GfxRenderer.h>

#include <algorithm>
#include <cstring>

std::vector<Hyphenator::BreakInfo> Hyphenator::breakOffsets(const std::string&, bool) { return {}; }

namespace BidiUtils {
bool startsWithRtl(const char*, int) { return false; }
int detectParagraphLevel(const char*, int fallbackLevel, int) { return fallbackLevel; }
bool computeVisualWordOrder(const std::vector<std::string>& words, bool, std::vector<uint16_t>& order) {
  order.resize(words.size());
  for (size_t index = 0; index < words.size(); ++index) order[index] = static_cast<uint16_t>(index);
  return true;
}
}  // namespace BidiUtils

TextBlock::TextBlock(const std::vector<std::string>& words, const std::vector<int16_t>& wordXpos,
                     const std::vector<EpdFontFamily::Style>&, const std::vector<uint8_t>&,
                     const std::vector<uint16_t>&, const std::vector<uint16_t>&, const std::vector<uint8_t>& wordFlags,
                     const std::vector<bool>&, const BlockStyle& blockStyle, std::vector<std::string> rubyTexts)
    : blockStyle(blockStyle), numWords(static_cast<uint16_t>(words.size())), rubyTexts(std::move(rubyTexts)) {
  if (wordXpos.empty()) return;
  // Positions are always kept. Word text and flags (which carry link ids) are kept too when
  // there is one entry per word, so tests can see what each line shows.
  // Arena layout: int16 positions, then uint16 text offsets, flags, and NUL-terminated text.
  const bool keepText = words.size() == wordXpos.size();
  const bool keepFlags = keepText && wordFlags.size() == words.size();
  const size_t positionBytes = wordXpos.size() * sizeof(int16_t);
  const size_t offsetBytes = keepText ? words.size() * sizeof(uint16_t) : 0;
  const size_t flagBytes = keepFlags ? wordFlags.size() : 0;
  size_t textSize = 0;
  if (keepText) {
    for (const auto& word : words) textSize += word.size() + 1;
  }
  arena = std::make_unique<uint8_t[]>(positionBytes + offsetBytes + flagBytes + textSize);
  uint8_t* cursor = arena.get();

  auto* positions = reinterpret_cast<int16_t*>(cursor);
  std::copy(wordXpos.begin(), wordXpos.end(), positions);
  xposArr = positions;
  cursor += positionBytes;
  if (!keepText) return;

  auto* offsets = reinterpret_cast<uint16_t*>(cursor);
  cursor += offsetBytes;

  if (keepFlags) {
    std::copy(wordFlags.begin(), wordFlags.end(), cursor);
    wordFlagsArr = cursor;
    wordFlagsPresent = true;
    cursor += flagBytes;
  }

  auto* text = reinterpret_cast<char*>(cursor);
  uint16_t offset = 0;
  for (size_t i = 0; i < words.size(); ++i) {
    offsets[i] = offset;
    std::memcpy(text + offset, words[i].c_str(), words[i].size() + 1);
    offset = static_cast<uint16_t>(offset + words[i].size() + 1);
  }
  textOffArr = offsets;
  textArr = text;
  textBytes = offset;
}
bool TextBlock::hasRuby() const { return false; }

bool ImageDecoderFactory::isFormatSupported(const std::string& path) { return path.ends_with(".jpg"); }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) { return nullptr; }
size_t ImageDimsProbe::write(uint8_t) { return 0; }
size_t ImageDimsProbe::write(const uint8_t*, size_t) { return 0; }
bool ImageDimsProbe::getDimensions(ImageDimensions&) const { return false; }

ImageBlock::ImageBlock(std::string imagePath, std::string sourcePath, const int16_t width, const int16_t height)
    : imagePath(std::move(imagePath)), sourcePath(std::move(sourcePath)), width(width), height(height) {}

void PageImage::render(GfxRenderer&, int, int, int, bool) {}
void PageImage::renderPlaceholder(GfxRenderer&, int, int, bool) const {}
bool PageImage::serialize(FsFile&) { return false; }

PreviewBlockLocator::PreviewBlockLocator(const char*, IsBlockTagFn) {}
PreviewBlockLocator::~PreviewBlockLocator() = default;
bool PreviewBlockLocator::feed(const char*, int, bool) { return false; }

CompactTableLayout::CompactTableLayout(GfxRenderer& renderer, int, uint16_t, uint16_t, uint16_t, uint8_t,
                                       BlockStyle tableStyle)
    : renderer_(renderer), tableStyle_(tableStyle) {}
bool CompactTableLayout::beginRow() { return true; }
bool CompactTableLayout::beginCell(bool, uint8_t, uint32_t, const BlockStyle&) { return true; }
bool CompactTableLayout::appendWord(std::string_view, EpdFontFamily::Style, bool, bool, uint8_t) { return true; }
bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>&) { return true; }
CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow&, std::vector<std::shared_ptr<TextBlock>>&,
                                                            std::vector<FootnoteEntry>&, uint32_t&) {
  return RowResult::Ok;
}

void PageLine::render(GfxRenderer&, int, int, int, bool) {}
bool PageLine::serialize(FsFile&) { return false; }
void PageHorizontalRule::render(GfxRenderer&, int, int, int, bool) {}
bool PageHorizontalRule::serialize(FsFile&) { return false; }
void PageTableFragment::render(GfxRenderer&, int, int, int, bool) {}
bool PageTableFragment::serialize(FsFile&) { return false; }
