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
                     const std::vector<bool>&, const BlockStyle& blockStyle, std::vector<std::string> rubyTexts,
                     const std::vector<uint8_t>& wordSizes, const char*, int8_t characterSpacing,
                     const std::vector<uint32_t>& fontFamilies, const std::vector<SourceRange>& sourceRanges)
    : blockStyle(blockStyle),
      characterSpacing(characterSpacing),
      numWords(static_cast<uint16_t>(words.size())),
      rubyTexts(std::move(rubyTexts)) {
  if (!fontFamilies.empty()) {
    wordFamilies = std::make_unique<uint32_t[]>(fontFamilies.size());
    std::copy(fontFamilies.begin(), fontFamilies.end(), wordFamilies.get());
  }
  if (wordXpos.empty()) return;
  // Arena layout: source ranges, int16 positions, uint16 text offsets, flags, sizes, then
  // NUL-terminated word text. Text is kept when there is one entry per word, so tests can see
  // what each laid-out line shows.
  const bool keepText = words.size() == wordXpos.size();
  const size_t sourceBytes = sourceRanges.size() * sizeof(SourceRange);
  const size_t positionBytes = wordXpos.size() * sizeof(int16_t);
  const size_t offsetBytes = keepText ? words.size() * sizeof(uint16_t) : 0;
  size_t textSize = 0;
  if (keepText) {
    for (const auto& word : words) textSize += word.size() + 1;
  }
  arena = std::make_unique<uint8_t[]>(sourceBytes + positionBytes + offsetBytes + wordFlags.size() + wordSizes.size() +
                                      textSize);
  uint8_t* cursor = arena.get();
  if (sourceBytes) {
    sourceRangesArr = reinterpret_cast<const SourceRange*>(cursor);
    std::memcpy(cursor, sourceRanges.data(), sourceBytes);
    sourceRangesPresent = true;
  }
  cursor += sourceBytes;
  auto* positions = reinterpret_cast<int16_t*>(cursor);
  std::copy(wordXpos.begin(), wordXpos.end(), positions);
  xposArr = positions;
  cursor += positionBytes;
  auto* offsets = reinterpret_cast<uint16_t*>(cursor);
  cursor += offsetBytes;
  if (!wordFlags.empty()) {
    std::copy(wordFlags.begin(), wordFlags.end(), cursor);
    wordFlagsArr = cursor;
    wordFlagsPresent = true;
  }
  cursor += wordFlags.size();
  if (!wordSizes.empty()) {
    std::copy(wordSizes.begin(), wordSizes.end(), cursor);
    wordSizesArr = cursor;
    wordSizesPresent = true;
  }
  cursor += wordSizes.size();
  if (!keepText) return;

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
namespace {
class FixtureImageDecoder final : public ImageToFramebufferDecoder {
 public:
  bool decodeToFramebuffer(const std::string&, GfxRenderer&, const RenderConfig&) override { return false; }
  bool getDimensions(const std::string&, ImageDimensions& dims) const override {
    GfxRenderer::fileProbeHadLoan = GfxRenderer::loanActive;
    dims = {800, 200};
    return true;
  }
  const char* getFormatName() const override { return "JPEG"; }
};
}  // namespace
bool ImageDecoderFactory::supportsMemorySource(const std::string&) { return false; }
ImageToFramebufferDecoder* ImageDecoderFactory::getDecoder(const std::string&) {
  static FixtureImageDecoder decoder;
  return &decoder;
}

ImageBlock::ImageBlock(std::string imagePath, std::string sourcePath, const int16_t width, const int16_t height)
    : imagePath(std::move(imagePath)), sourcePath(std::move(sourcePath)), width(width), height(height) {}

void PageImage::render(GfxRenderer&, int, int, int, bool) {}
void PageImage::renderPlaceholder(GfxRenderer&, int, int, bool) const {}
bool PageImage::serialize(Print&) { return false; }

PreviewBlockLocator::PreviewBlockLocator(const char*, IsBlockTagFn) {}
PreviewBlockLocator::~PreviewBlockLocator() = default;
bool PreviewBlockLocator::feed(const char*, int, bool) { return false; }

CompactTableLayout::CompactTableLayout(GfxRenderer& renderer, int, uint16_t, uint16_t, uint16_t, uint8_t,
                                       BlockStyle tableStyle, int8_t characterSpacing)
    : renderer_(renderer), tableStyle_(tableStyle), characterSpacing_(characterSpacing) {}
bool CompactTableLayout::beginRow() { return true; }
bool CompactTableLayout::beginCell(bool, uint8_t, uint32_t, const BlockStyle&) { return true; }
bool CompactTableLayout::appendWord(std::string_view, EpdFontFamily::Style, bool, bool, uint8_t, uint32_t, bool, bool) {
  return true;
}
bool CompactTableLayout::endCell(const std::vector<std::pair<int, FootnoteEntry>>&) { return true; }
CompactTableLayout::RowResult CompactTableLayout::finishRow(TableFragmentRow&, std::vector<std::shared_ptr<TextBlock>>&,
                                                            std::vector<FootnoteEntry>&, uint32_t&) {
  return RowResult::Ok;
}

void PageLine::render(GfxRenderer&, int, int, int, bool) {}
bool PageLine::serialize(Print&) { return false; }
void PageHorizontalRule::render(GfxRenderer&, int, int, int, bool) {}
bool PageHorizontalRule::serialize(Print&) { return false; }
void PageTableFragment::render(GfxRenderer&, int, int, int, bool) {}
bool PageTableFragment::serialize(Print&) { return false; }

PageDropCap::PageDropCap(uint8_t size, uint16_t scale, EpdFontFamily::Style style, const char* source, int16_t x,
                         int16_t y, uint32_t family)
    : PageElement(x, y), fontSize(size), fontFamily(family), scale256(scale), style(style) {
  std::strncpy(text, source, MAX_TEXT_BYTES);
}
void PageDropCap::render(GfxRenderer&, int, int, int, bool) {}
bool PageDropCap::serialize(Print&) { return false; }
void PageBorderBox::render(GfxRenderer&, int, int, int, bool) {}
bool PageBorderBox::serialize(Print&) { return false; }

int TextBlock::resolvedFontId(const GfxRenderer& r, int font) const {
  return r.getFontIdForSize(font, blockStyle.fontSize);
}
int TextBlock::wordFontId(const GfxRenderer& r, int font, uint16_t i) const {
  return r.resolveBookFont(resolvedFontId(r, font), wordFamilies ? wordFamilies[i] : 0, wordFontSize(i));
}
int TextBlock::maxAscender(const GfxRenderer& r, int font) const {
  int result = r.getFontAscenderSize(resolvedFontId(r, font));
  for (uint16_t i = 0; i < numWords; ++i) result = std::max(result, r.getFontAscenderSize(wordFontId(r, font, i)));
  return result;
}
RuledLineLayout TextBlock::ruledLayout(const GfxRenderer& r, int font, int requested) const {
  return RuledLineLayout::center(r, font, requested, {0, r.getFontAscenderSize(font)});
}
int TextBlock::maxLineHeight(const GfxRenderer& r, int font) const {
  int result = r.getLineHeight(resolvedFontId(r, font));
  for (uint16_t i = 0; i < numWords; ++i) result = std::max(result, r.getLineHeight(wordFontId(r, font, i)));
  return result;
}
