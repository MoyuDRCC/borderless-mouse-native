#include "core/clipboard/ClipboardContent.h"

#include <windows.h>
#include <shlobj.h>
#include <wincodec.h>
#include <wrl/client.h>

#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <utility>

namespace bm {
namespace {

using Microsoft::WRL::ComPtr;

uint32_t ReadBigEndian32(std::span<const uint8_t> buffer, size_t offset) {
  return (static_cast<uint32_t>(buffer[offset]) << 24) | (static_cast<uint32_t>(buffer[offset + 1]) << 16) |
         (static_cast<uint32_t>(buffer[offset + 2]) << 8) | static_cast<uint32_t>(buffer[offset + 3]);
}

void Check(HRESULT result, const char* message) {
  if (FAILED(result)) throw std::runtime_error(message);
}

// WIC 可在界面或接收线程使用，COM 由本线程配对初始化和释放。
class ComScope {
 public:
  ComScope() {
    const HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    owned_ = SUCCEEDED(result);
    if (FAILED(result) && result != RPC_E_CHANGED_MODE) throw std::runtime_error("无法初始化图像编解码组件");
  }
  ~ComScope() {
    if (owned_) CoUninitialize();
  }
  ComScope(const ComScope&) = delete;
  ComScope& operator=(const ComScope&) = delete;

 private:
  bool owned_ = false;
};

ComPtr<IWICImagingFactory> CreateImagingFactory() {
  ComPtr<IWICImagingFactory> factory;
  Check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
        "无法创建 WIC 图像工厂");
  return factory;
}

constexpr size_t kPngHeaderSize = 33;

// 全局内存在后台分配并填充，发布成功后由系统剪贴板接管。
class ClipboardBlock {
 public:
  ClipboardBlock(UINT format, size_t size) : format_(format) {
    if (!format_) throw std::runtime_error("无法注册剪贴板格式");
    block_ = GlobalAlloc(GMEM_MOVEABLE, size);
    if (!block_) throw std::runtime_error("无法分配剪贴板内存");
  }
  ClipboardBlock(UINT format, std::span<const uint8_t> data) : ClipboardBlock(format, data.size()) {
    Fill([&](uint8_t* target) { std::memcpy(target, data.data(), data.size()); });
  }
  ~ClipboardBlock() {
    if (block_) GlobalFree(block_);
  }
  ClipboardBlock(const ClipboardBlock&) = delete;
  ClipboardBlock& operator=(const ClipboardBlock&) = delete;

  template <typename Write>
  void Fill(Write write) {
    auto* target = static_cast<uint8_t*>(GlobalLock(block_));
    if (!target) throw std::runtime_error("无法锁定剪贴板内存");
    try {
      write(target);
    } catch (...) {
      GlobalUnlock(block_);
      throw;
    }
    GlobalUnlock(block_);
  }

  void Publish() {
    if (!SetClipboardData(format_, block_)) throw std::runtime_error("系统拒绝写入剪贴板");
    block_ = nullptr;
  }

 private:
  UINT format_;
  HGLOBAL block_ = nullptr;
};

}  // namespace

ClipboardSnapshot::ClipboardSnapshot(int64_t version, ClipboardOrigin origin, std::shared_ptr<const ClipboardContent> content)
    : version_(version),
      origin_(origin),
      content_(std::move(content)),
      revoked_(std::make_shared<std::atomic<bool>>(false)) {}

void ClipboardImageBudget::ValidateDimensions(uint32_t width, uint32_t height) {
  // 覆盖常见 8K 截图；宽高和乘积分别约束，避免细长图绕过像素预算
  if (width == 0 || height == 0 || width > kMaxDimension || height > kMaxDimension ||
      static_cast<uint64_t>(width) * height > kMaxPixels)
    throw std::runtime_error("图片须为单边不超过16384像素、总像素不超过33554432的有效尺寸");
}

void ClipboardImageBudget::ValidatePng(std::span<const uint8_t> png) {
  if (static_cast<int64_t>(png.size()) > net::kMaxContentLength) throw std::runtime_error("图片超过64MiB资源上限");
  // PNG 首块必须是 13 字节 IHDR；在任何解码或位图分配之前读取其大端尺寸
  static constexpr uint8_t kSignature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (png.size() < kPngHeaderSize || std::memcmp(png.data(), kSignature, sizeof(kSignature)) != 0 ||
      ReadBigEndian32(png, 8) != 13 || std::memcmp(png.data() + 12, "IHDR", 4) != 0)
    throw std::runtime_error("图片文档缺少有效PNG头");
  ValidateDimensions(ReadBigEndian32(png, 16), ReadBigEndian32(png, 20));
}

namespace {

std::unique_ptr<ClipboardBlock> PrepareDib(std::span<const uint8_t> png,
                                           const std::function<void()>& checkCanceled) {
  ClipboardImageBudget::ValidatePng(png);
  ComScope com;
  const ComPtr<IWICImagingFactory> factory = CreateImagingFactory();

  ComPtr<IWICStream> stream;
  Check(factory->CreateStream(&stream), "无法创建 PNG 内存流");
  // InitializeFromMemory 要求可写指针，解码过程只读
  Check(stream->InitializeFromMemory(const_cast<BYTE*>(png.data()), static_cast<DWORD>(png.size())),
        "无法打开 PNG 内存流");
  ComPtr<IWICBitmapDecoder> decoder;
  Check(factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
        "无法解码 PNG 文档");
  ComPtr<IWICBitmapFrameDecode> frame;
  Check(decoder->GetFrame(0, &frame), "PNG 文档没有图像帧");
  ComPtr<IWICFormatConverter> converter;
  Check(factory->CreateFormatConverter(&converter), "无法创建像素格式转换器");
  Check(converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0,
                              WICBitmapPaletteTypeCustom),
        "无法转换为 BGRA 像素");

  UINT width = 0;
  UINT height = 0;
  Check(converter->GetSize(&width, &height), "无法读取图片尺寸");
  ClipboardImageBudget::ValidateDimensions(width, height);

  // CF_DIB 用 40 字节 BITMAPINFOHEADER 加自底向上的 BGRA，兼容只认位图的程序
  const size_t stride = static_cast<size_t>(width) * 4;
  auto bitmap = std::make_unique<ClipboardBlock>(CF_DIB, sizeof(BITMAPINFOHEADER) + stride * height);
  // WIC 直接填充待交给剪贴板的内存，避免另外保留一份解码后的全尺寸位图。
  bitmap->Fill([&](uint8_t* data) {
    BITMAPINFOHEADER header{};
    header.biSize = sizeof(header);
    header.biWidth = static_cast<LONG>(width);
    header.biHeight = static_cast<LONG>(height);
    header.biPlanes = 1;
    header.biBitCount = 32;
    header.biCompression = BI_RGB;
    header.biSizeImage = static_cast<DWORD>(stride * height);
    std::memcpy(data, &header, sizeof(header));

    uint8_t* pixels = data + sizeof(header);
    for (UINT row = 0; row < height; row++) {
      if (row % 32 == 0) checkCanceled();
      const WICRect rect{0, static_cast<INT>(row), static_cast<INT>(width), 1};
      uint8_t* target = pixels + stride * (height - 1 - row);
      Check(converter->CopyPixels(&rect, static_cast<UINT>(stride), static_cast<UINT>(stride), target),
            "无法复制图片像素");
    }
  });
  return bitmap;
}

}  // namespace

Bytes PngFromDib(std::span<const uint8_t> dib) {
  if (dib.size() < sizeof(BITMAPINFOHEADER)) throw std::runtime_error("位图数据缺少 BITMAPINFOHEADER");
  BITMAPINFOHEADER header{};
  std::memcpy(&header, dib.data(), sizeof(header));
  if (header.biSize != sizeof(BITMAPINFOHEADER) || header.biCompression != BI_RGB ||
      (header.biBitCount != 24 && header.biBitCount != 32) || header.biWidth <= 0 || header.biHeight == 0)
    throw std::runtime_error("只支持24/32位未压缩位图");
  const uint32_t width = static_cast<uint32_t>(header.biWidth);
  const bool bottomUp = header.biHeight > 0;
  const uint32_t height = static_cast<uint32_t>(bottomUp ? header.biHeight : -header.biHeight);
  ClipboardImageBudget::ValidateDimensions(width, height);

  const size_t sourceStride = (static_cast<size_t>(width) * header.biBitCount + 31) / 32 * 4;
  if (dib.size() < sizeof(BITMAPINFOHEADER) + sourceStride * height) throw std::runtime_error("位图数据长度不足");
  const size_t targetStride = static_cast<size_t>(width) * 4;
  Bytes pixels(targetStride * height);
  const uint8_t* source = dib.data() + sizeof(BITMAPINFOHEADER);
  const size_t bytesPerPixel = header.biBitCount / 8;
  for (uint32_t row = 0; row < height; row++) {
    const uint8_t* line = source + sourceStride * (bottomUp ? height - 1 - row : row);
    uint8_t* target = pixels.data() + targetStride * row;
    for (uint32_t column = 0; column < width; column++) {
      target[column * 4] = line[column * bytesPerPixel];
      target[column * 4 + 1] = line[column * bytesPerPixel + 1];
      target[column * 4 + 2] = line[column * bytesPerPixel + 2];
      target[column * 4 + 3] = 0xFF;  // DIB 没有可靠透明度，按不透明处理
    }
  }

  ComScope com;
  const ComPtr<IWICImagingFactory> factory = CreateImagingFactory();
  ComPtr<IWICBitmap> bitmap;
  Check(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, static_cast<UINT>(targetStride),
                                        static_cast<UINT>(pixels.size()), pixels.data(), &bitmap),
        "无法从位图创建图像");
  ComPtr<IStream> stream;
  Check(CreateStreamOnHGlobal(nullptr, TRUE, &stream), "无法创建 PNG 输出流");
  ComPtr<IWICBitmapEncoder> encoder;
  Check(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "无法创建 PNG 编码器");
  Check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "无法初始化 PNG 编码器");
  ComPtr<IWICBitmapFrameEncode> frame;
  ComPtr<IPropertyBag2> properties;
  Check(encoder->CreateNewFrame(&frame, &properties), "无法创建 PNG 帧");
  Check(frame->Initialize(properties.Get()), "无法初始化 PNG 帧");
  Check(frame->SetSize(width, height), "无法设置 PNG 尺寸");
  WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
  Check(frame->SetPixelFormat(&format), "无法设置 PNG 像素格式");
  Check(frame->WriteSource(bitmap.Get(), nullptr), "无法写入 PNG 像素");
  Check(frame->Commit(), "无法提交 PNG 帧");
  Check(encoder->Commit(), "无法完成 PNG 编码");

  STATSTG status{};
  Check(stream->Stat(&status, STATFLAG_NONAME), "无法读取 PNG 流大小");
  HGLOBAL block = nullptr;
  Check(GetHGlobalFromStream(stream.Get(), &block), "无法读取 PNG 输出流");
  const size_t size = static_cast<size_t>(status.cbSize.QuadPart);
  Bytes png(size);
  void* data = GlobalLock(block);
  std::memcpy(png.data(), data, size);
  GlobalUnlock(block);
  return png;
}

namespace {

// 打开时关联有效窗口，剪贴板完成提交后才释放独占访问。
class ClipboardSession {
 public:
  explicit ClipboardSession(HWND owner) : opened_(OpenClipboard(owner) != FALSE) {}
  ~ClipboardSession() {
    if (opened_) CloseClipboard();
  }
  ClipboardSession(const ClipboardSession&) = delete;
  ClipboardSession& operator=(const ClipboardSession&) = delete;

  bool Opened() const { return opened_; }

 private:
  bool opened_;
};

std::vector<wchar_t> TerminatedText(std::wstring_view text) {
  std::vector<wchar_t> buffer(text.begin(), text.end());
  buffer.push_back(L'\0');
  return buffer;
}

Bytes FileDropPayload(const std::vector<std::wstring>& paths) {
  if (paths.empty()) throw std::invalid_argument("剪贴板文件清单为空");
  // CF_HDROP：DROPFILES 头加双 NUL 结尾的路径表
  std::vector<wchar_t> list;
  for (const std::wstring& path : paths) {
    list.insert(list.end(), path.begin(), path.end());
    list.push_back(L'\0');
  }
  list.push_back(L'\0');
  DROPFILES drop{};
  drop.pFiles = sizeof(DROPFILES);
  drop.fWide = TRUE;
  Bytes payload(sizeof(DROPFILES) + list.size() * sizeof(wchar_t));
  std::memcpy(payload.data(), &drop, sizeof(drop));
  std::memcpy(payload.data() + sizeof(DROPFILES), list.data(), list.size() * sizeof(wchar_t));
  return payload;
}

}  // namespace

struct ClipboardWriter::Prepared::Formats {
  std::vector<std::unique_ptr<ClipboardBlock>> blocks;
};

ClipboardWriter::Prepared::Prepared(std::unique_ptr<Formats> formats) : formats_(std::move(formats)) {}
ClipboardWriter::Prepared::Prepared(Prepared&& other) noexcept = default;
ClipboardWriter::Prepared& ClipboardWriter::Prepared::operator=(Prepared&& other) noexcept = default;
ClipboardWriter::Prepared::~Prepared() = default;

ClipboardWriteResult ClipboardWriter::Prepared::Commit(HWND owner, uint32_t& sequence) {
  if (!owner) throw std::runtime_error("剪贴板窗口已关闭");
  ClipboardSession session(owner);
  if (!session.Opened()) return ClipboardWriteResult::Busy;
  // 独占期间最后核对序列号，避免覆盖后台解码期间复制的新内容。
  if (GetClipboardSequenceNumber() != sequence) return ClipboardWriteResult::Changed;
  if (!EmptyClipboard()) throw std::runtime_error("无法清空剪贴板");
  for (const auto& block : formats_->blocks) block->Publish();
  sequence = GetClipboardSequenceNumber();
  return ClipboardWriteResult::Written;
}

bool ClipboardWriter::WriteLocalText(HWND owner, std::wstring_view text) {
  if (!owner) return false;
  const auto buffer = TerminatedText(text);
  ClipboardBlock content(CF_UNICODETEXT,
                         {reinterpret_cast<const uint8_t*>(buffer.data()), buffer.size() * sizeof(wchar_t)});
  ClipboardSession session(owner);
  if (!session.Opened() || !EmptyClipboard()) return false;
  content.Publish();
  return true;
}

ClipboardWriter::Prepared ClipboardWriter::Prepare(const ClipboardContent& content,
                                                    const std::function<void()>& checkCanceled) {
  checkCanceled();
  auto formats = std::make_unique<Prepared::Formats>();
  const uint32_t marker = 1;
  formats->blocks.push_back(std::make_unique<ClipboardBlock>(
      RegisterClipboardFormatW(kRemoteMarker),
      std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(&marker), sizeof(marker))));

  switch (content.kind) {
    case net::ClipboardContentKind::Text: {
      const auto buffer = TerminatedText(ToWide(content.text));
      formats->blocks.push_back(std::make_unique<ClipboardBlock>(
          CF_UNICODETEXT, std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(buffer.data()),
                                                  buffer.size() * sizeof(wchar_t))));
      break;
    }
    case net::ClipboardContentKind::Image: {
      auto bitmap = PrepareDib(content.image, checkCanceled);
      checkCanceled();
      formats->blocks.push_back(std::make_unique<ClipboardBlock>(RegisterClipboardFormatW(kPngFormat), content.image));
      formats->blocks.push_back(std::move(bitmap));
      break;
    }
    case net::ClipboardContentKind::Files: {
      const Bytes payload = FileDropPayload(content.paths);
      formats->blocks.push_back(std::make_unique<ClipboardBlock>(CF_HDROP, payload));
      break;
    }
  }
  checkCanceled();
  return Prepared(std::move(formats));
}

bool ClipboardWriter::HasRemoteMarker() {
  return IsClipboardFormatAvailable(RegisterClipboardFormatW(kRemoteMarker)) != FALSE;
}

}  // namespace bm
