class Frametap < Formula
  desc "Cross-platform screen capture CLI for macOS, Linux, and Windows"
  homepage "https://github.com/krazyjakee/frametap"
  license "MIT"
  version "0.1.3"

  on_macos do
    if Hardware::CPU.arm?
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-arm64.zip"
      sha256 "a17a28c7e732a743fc6e9424cbc11c559785a3bccc8160cf24dbdf439e060156" # macos-arm64
    else
      url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-macos-x86_64.zip"
      sha256 "149ac3e9a1069de293889b766c644300ca1f791d46c81def29e6c54bd3a58111" # macos-x86_64
    end
  end

  on_linux do
    url "https://github.com/krazyjakee/frametap/releases/download/v#{version}/frametap-cli-linux.zip"
    sha256 "fd75aa4f0c2d198307c25225171373d02dc7962dc24126571d662f8ff99fdd46" # linux
  end

  def install
    bin.install "frametap"
  end

  test do
    assert_match version.to_s, shell_output("#{bin}/frametap --version")
  end
end
