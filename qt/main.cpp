#include "qt/info_dialog.hpp"
#include "qt/mainwindow.hpp"
#include "qt/screenshoter.hpp"

#include "map/framework.hpp"

#include "platform/platform.hpp"
#include "platform/settings.hpp"

#include "coding/file_reader.hpp"

#include "base/logging.hpp"
#include "base/macros.hpp"

#include "build_style/build_style.h"

#include "std/cstdio.hpp"
#include "std/cstdlib.hpp"
#include "std/sstream.hpp"

#include "3party/Alohalytics/src/alohalytics.h"
#include "3party/gflags/src/gflags/gflags.h"

#include <QtCore/QDir>
#include <QtWidgets/QMessageBox>
#include <QtWidgets/QApplication>
#include <QtWidgets/QFileDialog>

DEFINE_string(data_path, "", "Path to data directory.");
DEFINE_string(log_abort_level, base::ToString(base::GetDefaultLogAbortLevel()),
              "Log messages severity that causes termination.");
DEFINE_string(resources_path, "", "Path to resources directory.");
DEFINE_string(kml_path, "", "Path to a directory with kml files to take screenshots.");
DEFINE_string(dst_path, "", "Path to a directory to save screenshots.");
DEFINE_string(lang, "", "Device language.");
DEFINE_int32(width, 0, "Screenshot width.");
DEFINE_int32(height, 0, "Screenshot height.");
DEFINE_double(dpi_scale, 0.0, "Screenshot dpi scale.");

namespace
{
bool ValidateLogAbortLevel(char const * flagname, string const & value)
{
  base::LogLevel level;
  if (!base::FromString(value, level))
  {
    ostringstream os;
    auto const & names = base::GetLogLevelNames();
    for (size_t i = 0; i < names.size(); ++i)
    {
      if (i != 0)
        os << ", ";
      os << names[i];
    }

    printf("Invalid value for --%s: %s, must be one of: %s\n", flagname, value.c_str(),
           os.str().c_str());
    return false;
  }
  return true;
}

bool const g_logAbortLevelDummy =
    google::RegisterFlagValidator(&FLAGS_log_abort_level, &ValidateLogAbortLevel);

class FinalizeBase
{
public:
  ~FinalizeBase()
  {
    // optional - clean allocated data in protobuf library
    // useful when using memory and resource leak utilites
    // google::protobuf::ShutdownProtobufLibrary();
  }
  };

#if defined(OMIM_OS_WINDOWS) //&& defined(PROFILER_COMMON)
  class InitializeFinalize : public FinalizeBase
  {
    FILE * m_errFile;
    base::ScopedLogLevelChanger const m_debugLog;
  public:
    InitializeFinalize() : m_debugLog(LDEBUG)
    {
      // App runs without error console under win32.
      m_errFile = ::freopen(".\\mapsme.log", "w", stderr);

      //_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_DELAY_FREE_MEM_DF);
      //_CrtSetDbgFlag(_CRTDBG_ALLOC_MEM_DF | _CRTDBG_LEAK_CHECK_DF);
    }
    ~InitializeFinalize()
    {
      ::fclose(m_errFile);
    }
  };
#else
  typedef FinalizeBase InitializeFinalize;
#endif
}  // namespace

int main(int argc, char * argv[])
{
  google::SetUsageMessage("Desktop application.");
  google::ParseCommandLineFlags(&argc, &argv, true);

  Platform & platform = GetPlatform();
  if (!FLAGS_resources_path.empty())
    platform.SetResourceDir(FLAGS_resources_path);
  if (!FLAGS_data_path.empty())
    platform.SetWritableDirForTests(FLAGS_data_path);

  base::LogLevel level;
  CHECK(base::FromString(FLAGS_log_abort_level, level), ());
  base::g_LogAbortLevel = level;

  Q_INIT_RESOURCE(resources_common);

  // Our double parsing code (base/string_utils.hpp) needs dots as a floating point delimiters, not commas.
  // TODO: Refactor our doubles parsing code to use locale-independent delimiters.
  // For example, https://github.com/google/double-conversion can be used.
  // See http://dbaron.org/log/20121222-locale for more details.
  (void)::setenv("LC_NUMERIC", "C", 1);

  InitializeFinalize mainGuard;
  UNUSED_VALUE(mainGuard);

  QApplication a(argc, argv);

#ifdef DEBUG
  alohalytics::Stats::Instance().SetDebugMode(true);
#endif

  platform.SetupMeasurementSystem();

  // display EULA if needed
  char const * settingsEULA = "EulaAccepted";
  bool eulaAccepted = false;
  if (!settings::Get(settingsEULA, eulaAccepted) || !eulaAccepted)
  {
    QStringList buttons;
    buttons << "Accept" << "Decline";

    string buffer;
    {
      ReaderPtr<Reader> reader = platform.GetReader("eula.html");
      reader.ReadAsString(buffer);
    }
    qt::InfoDialog eulaDialog(qAppName() + QString(" End User Licensing Agreement"), buffer.c_str(), NULL, buttons);
    eulaAccepted = (eulaDialog.exec() == 1);
    settings::Set(settingsEULA, eulaAccepted);
  }

  int returnCode = -1;
  QString mapcssFilePath;
  if (eulaAccepted)   // User has accepted EULA
  {
    bool apiOpenGLES3 = false;
    std::unique_ptr<qt::ScreenshotParams> screenshotParams;

#if defined(OMIM_OS_MAC)
    apiOpenGLES3 = a.arguments().contains("es3", Qt::CaseInsensitive);

    if (!FLAGS_lang.empty())
      (void)::setenv("LANGUAGE", FLAGS_lang.c_str(), 1);

    if (!FLAGS_kml_path.empty())
    {
      screenshotParams = std::make_unique<qt::ScreenshotParams>();
      screenshotParams->m_kmlPath = FLAGS_kml_path;
      if (!FLAGS_dst_path.empty())
        screenshotParams->m_dstPath = FLAGS_dst_path;
      if (FLAGS_width > 0)
        screenshotParams->m_width = FLAGS_width;
      if (FLAGS_height > 0)
        screenshotParams->m_height = FLAGS_height;
      if (FLAGS_dpi_scale >= df::VisualParams::kMdpiScale && FLAGS_dpi_scale <= df::VisualParams::kXxxhdpiScale)
        screenshotParams->m_dpiScale = FLAGS_dpi_scale;
    }
#endif
    qt::MainWindow::SetDefaultSurfaceFormat(apiOpenGLES3);

    FrameworkParams frameworkParams;

#ifdef BUILD_DESIGNER
    if (argc >= 2 && platform.IsFileExistsByFullPath(argv[1]))
        mapcssFilePath = argv[1];
    if (0 == mapcssFilePath.length())
    {
      mapcssFilePath = QFileDialog::getOpenFileName(nullptr, "Open style.mapcss file", "~/",
                                                    "MapCSS Files (*.mapcss)");
    }
    if (mapcssFilePath.isEmpty())
      return returnCode;

    try
    {
      build_style::BuildIfNecessaryAndApply(mapcssFilePath);
    }
    catch (std::exception const & e)
    {
      QMessageBox msgBox;
      msgBox.setWindowTitle("Error");
      msgBox.setText(e.what());
      msgBox.setStandardButtons(QMessageBox::Ok);
      msgBox.setDefaultButton(QMessageBox::Ok);
      msgBox.exec();
      return returnCode;
    }

    // Designer tool can regenerate geometry index, so local ads can't work.
    frameworkParams.m_enableLocalAds = false;
#endif // BUILD_DESIGNER

    Framework framework(frameworkParams);
    qt::MainWindow w(framework, apiOpenGLES3, std::move(screenshotParams), mapcssFilePath);
    w.show();
    returnCode = a.exec();
  }

#ifdef BUILD_DESIGNER
  if (build_style::NeedRecalculate && !mapcssFilePath.isEmpty())
  {
    try
    {
      build_style::RunRecalculationGeometryScript(mapcssFilePath);
    }
    catch (std::exception & e)
    {
      QMessageBox msgBox;
      msgBox.setWindowTitle("Error");
      msgBox.setText(e.what());
      msgBox.setStandardButtons(QMessageBox::Ok);
      msgBox.setDefaultButton(QMessageBox::Ok);
      msgBox.exec();
    }
  }
#endif // BUILD_DESIGNER

  LOG_SHORT(LINFO, ("MapsWithMe finished with code", returnCode));
  return returnCode;
}
