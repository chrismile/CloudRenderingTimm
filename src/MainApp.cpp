/*
 * BSD 2-Clause License
 *
 * Copyright (c) 2020, Christoph Neuhauser
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 *
 * * Redistributions of source code must retain the above copyright notice, this
 *   list of conditions and the following disclaimer.
 *
 * * Redistributions in binary form must reproduce the above copyright notice,
 *   this list of conditions and the following disclaimer in the documentation
 *   and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
 * AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
 * SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
 * CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,
 * OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#define GLM_ENABLE_EXPERIMENTAL
#include <algorithm>
#include <stack>
#include <csignal>

#include <glm/gtx/color_space.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/rotate_vector.hpp>
#include <GL/glew.h>
#include <boost/algorithm/string.hpp>

#ifdef USE_ZEROMQ
#include <zmq.h>
#endif

#include <Utils/Timer.hpp>
#include <Utils/AppSettings.hpp>
#include <Utils/File/Logfile.hpp>
#include <Utils/File/FileUtils.hpp>
#include <Input/Keyboard.hpp>
#include <Math/Math.hpp>
#include <Math/Geometry/MatrixUtil.hpp>
#include <Graphics/Window.hpp>
#include <Graphics/Renderer.hpp>
#include <Graphics/Vulkan/Utils/Instance.hpp>
#include <Graphics/Vulkan/Utils/ScreenshotReadbackHelper.hpp>

#include <ImGui/ImGuiWrapper.hpp>
#include <ImGui/ImGuiFileDialog/ImGuiFileDialog.h>
#include <ImGui/imgui_internal.h>
#include <ImGui/imgui_custom.h>
#include <ImGui/imgui_stdlib.h>

#ifdef SUPPORT_OPTIX
#include "Denoiser/OptixVptDenoiser.hpp"
#endif
#include "DataView.hpp"
#include "MainApp.hpp"

void vulkanErrorCallback() {
    std::cerr << "Application callback" << std::endl;
}

#ifdef __linux__
void signalHandler(int signum) {
    SDL_CaptureMouse(SDL_FALSE);
    std::cerr << "Interrupt signal (" << signum << ") received." << std::endl;
    exit(signum);
}
#endif

MainApp::MainApp()
        : sceneData(
                sceneFramebuffer, sceneTexture, sceneDepthRBO, camera, clearColor,
                screenshotTransparentBackground, recording, useCameraFlight) {
    sgl::AppSettings::get()->getVulkanInstance()->setDebugCallback(&vulkanErrorCallback);

#ifdef SUPPORT_OPTIX
    optixInitialized = OptixVptDenoiser::initGlobal();
#endif

    checkpointWindow.setStandardWindowSize(1254, 390);
    checkpointWindow.setStandardWindowPosition(841, 53);

    camera->setNearClipDistance(0.01f);
    camera->setFarClipDistance(100.0f);

    useDockSpaceMode = true;
    sgl::AppSettings::get()->getSettings().getValueOpt("useDockSpaceMode", useDockSpaceMode);
    sgl::AppSettings::get()->getSettings().getValueOpt("useFixedSizeViewport", useFixedSizeViewport);
    showPropertyEditor = useDockSpaceMode;
    sgl::ImGuiWrapper::get()->setUseDockSpaceMode(useDockSpaceMode);
    useDockSpaceMode = true;

#ifdef NDEBUG
    showFpsOverlay = false;
#else
    showFpsOverlay = true;
#endif
    sgl::AppSettings::get()->getSettings().getValueOpt("showFpsOverlay", showFpsOverlay);
    sgl::AppSettings::get()->getSettings().getValueOpt("showCoordinateAxesOverlay", showCoordinateAxesOverlay);

    useLinearRGB = false;
    coordinateAxesOverlayWidget.setClearColor(clearColor);

    if (usePerformanceMeasurementMode) {
        useCameraFlight = true;
    }
    if (useCameraFlight && recording) {
        sgl::Window *window = sgl::AppSettings::get()->getMainWindow();
        window->setWindowSize(recordingResolution.x, recordingResolution.y);
        realTimeCameraFlight = false;
    }

    fileDialogInstance = IGFD_Create();
    customDataSetFileName = sgl::FileUtils::get()->getUserDirectory();
    loadAvailableDataSetInformation();

    volumetricPathTracingPass = std::make_shared<VolumetricPathTracingPass>(rendererVk, &cameraHandle);
    volumetricPathTracingPass->setUseLinearRGB(useLinearRGB);
    volumetricPathTracingPass->setFileDialogInstance(fileDialogInstance);
    dataView = std::make_shared<DataView>(camera, rendererVk, volumetricPathTracingPass);
    if (useDockSpaceMode) {
        cameraHandle = dataView->camera;
    } else {
        cameraHandle = camera;
    }

    resolutionChanged(sgl::EventPtr());

    if (!recording && !usePerformanceMeasurementMode) {
        // Just for convenience...
        int desktopWidth = 0;
        int desktopHeight = 0;
        int refreshRate = 60;
        sgl::AppSettings::get()->getDesktopDisplayMode(desktopWidth, desktopHeight, refreshRate);
        if (desktopWidth == 3840 && desktopHeight == 2160) {
            sgl::Window *window = sgl::AppSettings::get()->getMainWindow();
            window->setWindowSize(2186, 1358);
        }
    }

    if (!sgl::AppSettings::get()->getSettings().hasKey("cameraNavigationMode")) {
        cameraNavigationMode = sgl::CameraNavigationMode::TURNTABLE;
        updateCameraNavigationMode();
    }

    usesNewState = true;
    recordingTimeStampStart = sgl::Timer->getTicksMicroseconds();

#ifdef __linux__
    signal(SIGSEGV, signalHandler);
#endif
}

MainApp::~MainApp() {
    device->waitIdle();

    volumetricPathTracingPass = {};
    dataView = {};

#ifdef SUPPORT_OPTIX
    if (optixInitialized) {
        OptixVptDenoiser::freeGlobal();
    }
#endif

    IGFD_Destroy(fileDialogInstance);

    sgl::AppSettings::get()->getSettings().addKeyValue("useDockSpaceMode", useDockSpaceMode);
    sgl::AppSettings::get()->getSettings().addKeyValue("useFixedSizeViewport", useFixedSizeViewport);
    sgl::AppSettings::get()->getSettings().addKeyValue("showFpsOverlay", showFpsOverlay);
    sgl::AppSettings::get()->getSettings().addKeyValue("showCoordinateAxesOverlay", showCoordinateAxesOverlay);
}

void MainApp::resolutionChanged(sgl::EventPtr event) {
    SciVisApp::resolutionChanged(event);

    sgl::Window *window = sgl::AppSettings::get()->getMainWindow();
    auto width = uint32_t(window->getWidth());
    auto height = uint32_t(window->getHeight());

    if (!useDockSpaceMode) {
        volumetricPathTracingPass->setOutputImage(sceneTextureVk->getImageView());
        volumetricPathTracingPass->recreateSwapchain(width, height);
    }
}

void MainApp::updateColorSpaceMode() {
    SciVisApp::updateColorSpaceMode();
    volumetricPathTracingPass->setUseLinearRGB(useLinearRGB);
}

void MainApp::render() {
    SciVisApp::preRender();

    if (!useDockSpaceMode) {
        reRender = reRender || volumetricPathTracingPass->needsReRender();

        if (reRender || continuousRendering) {
            SciVisApp::prepareReRender();

            if (cloudData) {
                volumetricPathTracingPass->render();
            }

            reRender = false;
        }
    }

    SciVisApp::postRender();
}

void MainApp::renderGui() {
    focusedWindowIndex = -1;
    mouseHoverWindowIndex = -1;

    if (sgl::Keyboard->keyPressed(SDLK_o) && (sgl::Keyboard->getModifier() & (KMOD_LCTRL | KMOD_RCTRL)) != 0) {
        openFileDialog();
    }

    if (IGFD_DisplayDialog(
            fileDialogInstance,
            "ChooseDataSetFile", ImGuiWindowFlags_NoCollapse,
            sgl::ImGuiWrapper::get()->getScaleDependentSize(1000, 580),
            ImVec2(FLT_MAX, FLT_MAX))) {
        if (IGFD_IsOk(fileDialogInstance)) {
            std::string filePathName = IGFD_GetFilePathName(fileDialogInstance);
            std::string filePath = IGFD_GetCurrentPath(fileDialogInstance);
            std::string filter = IGFD_GetCurrentFilter(fileDialogInstance);
            std::string userDatas;
            if (IGFD_GetUserDatas(fileDialogInstance)) {
                userDatas = std::string((const char*)IGFD_GetUserDatas(fileDialogInstance));
            }
            auto selection = IGFD_GetSelection(fileDialogInstance);

            // Is this line data set or a volume data file for the scattering line tracer?
            const char* currentPath = IGFD_GetCurrentPath(fileDialogInstance);
            std::string filename = currentPath;
            if (!filename.empty() && filename.back() != '/' && filename.back() != '\\') {
                filename += "/";
            }
            filename += selection.table[0].fileName;
            IGFD_Selection_DestroyContent(&selection);
            if (currentPath) {
                free((void*)currentPath);
                currentPath = nullptr;
            }

            std::string filenameLower = boost::to_lower_copy(filename);
            selectedDataSetIndex = 0;
            if (boost::ends_with(filenameLower, ".xyz")
                || boost::ends_with(filenameLower, ".nvdb")) {
                sgl::Logfile::get()->writeError("The selected file name has an unknown extension.");
            }
            customDataSetFileName = filename;
            loadCloudDataSet(getSelectedDataSetFilename());
        }
        IGFD_CloseDialog(fileDialogInstance);
    }

    if (useDockSpaceMode) {
        ImGuiID dockSpaceId = ImGui::DockSpaceOverViewport(ImGui::GetMainViewport());
        ImGuiDockNode* centralNode = ImGui::DockBuilderGetNode(dockSpaceId);
        static bool isProgramStartup = true;
        if (isProgramStartup && centralNode->IsEmpty()) {
            ImGuiID dockLeftId, dockMainId;
            ImGui::DockBuilderSplitNode(
                    dockSpaceId, ImGuiDir_Left, 0.3f,
                    &dockLeftId, &dockMainId);
            ImGui::DockBuilderDockWindow("Volumetric Path Tracer", dockMainId);

            ImGuiID dockLeftUpId, dockLeftDownId;
            ImGui::DockBuilderSplitNode(
                    dockLeftId, ImGuiDir_Up, 0.8f,
                    &dockLeftUpId, &dockLeftDownId);
            ImGui::DockBuilderDockWindow("Property Editor", dockLeftUpId);

            ImGui::DockBuilderDockWindow("Camera Checkpoints", dockLeftDownId);

            ImGui::DockBuilderFinish(dockLeftId);
            ImGui::DockBuilderFinish(dockSpaceId);
        }
        isProgramStartup = false;

        renderGuiMenuBar();

        if (showRendererWindow) {
            bool isViewOpen = true;
            sgl::ImGuiWrapper::get()->setNextWindowStandardSize(800, 600);
            if (ImGui::Begin("Volumetric Path Tracer", &isViewOpen)) {
                if (ImGui::IsWindowFocused()) {
                    focusedWindowIndex = 0;
                }
                sgl::ImGuiWrapper::get()->setWindowViewport(0, ImGui::GetWindowViewport());
                sgl::ImGuiWrapper::get()->setWindowViewport(0, ImGui::GetWindowViewport());
                sgl::ImGuiWrapper::get()->setWindowPosAndSize(0, ImGui::GetWindowPos(), ImGui::GetWindowSize());

                ImVec2 sizeContent = ImGui::GetContentRegionAvail();
                if (useFixedSizeViewport) {
                    sizeContent = ImVec2(float(fixedViewportSize.x), float(fixedViewportSize.y));
                }
                if (int(sizeContent.x) != int(dataView->viewportWidth)
                        || int(sizeContent.y) != int(dataView->viewportHeight)) {
                    dataView->resize(int(sizeContent.x), int(sizeContent.y));
                    if (dataView->viewportWidth > 0 && dataView->viewportHeight > 0) {

                        volumetricPathTracingPass->setOutputImage(dataView->dataViewTexture->getImageView());
                        volumetricPathTracingPass->recreateSwapchain(
                                dataView->viewportWidth, dataView->viewportHeight);

                    }
                    reRender = true;
                }

                reRender = reRender || volumetricPathTracingPass->needsReRender();

                if (reRender || continuousRendering) {
                    dataView->beginRender();
                    if (cloudData) {
                        volumetricPathTracingPass->render();
                    }
                    dataView->endRender();

                    reRender = false;
                }

                if (dataView->viewportWidth > 0 && dataView->viewportHeight > 0) {
                    if (!uiOnScreenshot && screenshot) {
                        printNow = true;
                        std::string screenshotFilename =
                                saveDirectoryScreenshots + saveFilenameScreenshots
                                + "_" + sgl::toString(screenshotNumber);
                        screenshotFilename += ".png";

                        customScreenshotWidth = int(dataView->viewportWidth);
                        customScreenshotHeight = int(dataView->viewportHeight);
                        auto tmp = compositedTextureVk;
                        compositedTextureVk = dataView->compositedDataViewTexture;

                        saveScreenshot(screenshotFilename);

                        compositedTextureVk = tmp;
                        customScreenshotWidth = -1;
                        customScreenshotHeight = -1;

                        sgl::Renderer->unbindFBO();
                        printNow = false;
                        screenshot = true;
                    }
                    if (!uiOnScreenshot && recording && !isFirstRecordingFrame) {
                        videoWriter->pushFramebufferImage(dataView->compositedDataViewTexture->getImage());
                    }

                    if (isViewOpen) {
                        ImTextureID textureId = dataView->getImGuiTextureId();
                        ImGui::Image(textureId, sizeContent, ImVec2(0, 0), ImVec2(1, 1));
                        if (ImGui::IsItemHovered()) {
                            mouseHoverWindowIndex = 0;
                        }
                    }

                    if (showFpsOverlay) {
                        renderGuiFpsOverlay();
                    }
                    if (showCoordinateAxesOverlay) {
                        renderGuiCoordinateAxesOverlay(dataView->camera);
                    }
                }
            }
            ImGui::End();
        }

        if (!uiOnScreenshot && screenshot) {
            screenshot = false;
            screenshotNumber++;
        }
        reRender = false;
    }

    if (checkpointWindow.renderGui()) {
        fovDegree = camera->getFOVy() / sgl::PI * 180.0f;
        reRender = true;
        hasMoved();
    }

    if (showPropertyEditor) {
        renderGuiPropertyEditorWindow();
    }
}

void MainApp::loadAvailableDataSetInformation() {
    dataSetNames.clear();
    dataSetNames.emplace_back("Local file...");
    selectedDataSetIndex = 0;

    const std::string lineDataSetsDirectory = sgl::AppSettings::get()->getDataDirectory() + "CloudDataSets/";
    if (sgl::FileUtils::get()->exists(lineDataSetsDirectory + "datasets.json")) {
        dataSetInformationRoot = loadDataSetList(lineDataSetsDirectory + "datasets.json");

        std::stack<std::pair<DataSetInformationPtr, size_t>> dataSetInformationStack;
        dataSetInformationStack.push(std::make_pair(dataSetInformationRoot, 0));
        while (!dataSetInformationStack.empty()) {
            std::pair<DataSetInformationPtr, size_t> dataSetIdxPair = dataSetInformationStack.top();
            DataSetInformationPtr dataSetInformationParent = dataSetIdxPair.first;
            size_t idx = dataSetIdxPair.second;
            dataSetInformationStack.pop();
            while (idx < dataSetInformationParent->children.size()) {
                DataSetInformationPtr dataSetInformationChild =
                        dataSetInformationParent->children.at(idx);
                idx++;
                if (dataSetInformationChild->type == DATA_SET_TYPE_NODE) {
                    dataSetInformationStack.push(std::make_pair(dataSetInformationRoot, idx));
                    dataSetInformationStack.push(std::make_pair(dataSetInformationChild, 0));
                    break;
                } else {
                    dataSetInformationChild->sequentialIndex = int(dataSetNames.size());
                    dataSetInformationList.push_back(dataSetInformationChild);
                    dataSetNames.push_back(dataSetInformationChild->name);
                }
            }
        }
    }
}

const std::string& MainApp::getSelectedDataSetFilename() {
    if (selectedDataSetIndex == 0) {
        return customDataSetFileName;
    }
    return dataSetInformationList.at(selectedDataSetIndex - NUM_MANUAL_LOADERS)->filename;
}

void MainApp::renderGuiGeneralSettingsPropertyEditor() {
    if (propertyEditor.addColorEdit3("Clear Color", (float*)&clearColorSelection, 0)) {
        clearColor = sgl::colorFromFloat(
                clearColorSelection.x, clearColorSelection.y, clearColorSelection.z, clearColorSelection.w);
        coordinateAxesOverlayWidget.setClearColor(clearColor);
        if (cloudData) {
            cloudData->setClearColor(clearColor);
        }
        reRender = true;
    }

    bool newDockSpaceMode = useDockSpaceMode;
    if (propertyEditor.addCheckbox("Use Docking Mode", &newDockSpaceMode)) {
        scheduledDockSpaceModeChange = true;
    }

    if (propertyEditor.addCheckbox("Fixed Size Viewport", &useFixedSizeViewport)) {
        reRender = true;
    }
    if (useFixedSizeViewport) {
        if (propertyEditor.addSliderInt2Edit("Viewport Size", &fixedViewportSizeEdit.x, 1, 8192)
                == ImGui::EditMode::INPUT_FINISHED) {
            fixedViewportSize = fixedViewportSizeEdit;
            reRender = true;
        }
    }
}

void MainApp::openFileDialog() {
    selectedDataSetIndex = 0;
    std::string fileDialogDirectory = sgl::AppSettings::get()->getDataDirectory() + "CloudDataSets/";
    if (!sgl::FileUtils::get()->exists(fileDialogDirectory)) {
        fileDialogDirectory = sgl::AppSettings::get()->getDataDirectory();
    }
    IGFD_OpenModal(
            fileDialogInstance,
            "ChooseDataSetFile", "Choose a File",
            ".*,.xyz,.nvdb",
            fileDialogDirectory.c_str(),
            "", 1, nullptr,
            ImGuiFileDialogFlags_ConfirmOverwrite);
}

void MainApp::renderGuiMenuBar() {
    if (ImGui::BeginMainMenuBar()) {
        if (ImGui::BeginMenu("File")) {
            if (ImGui::MenuItem("Open Dataset...", "CTRL+O")) {
                openFileDialog();
            }

            if (ImGui::BeginMenu("Datasets")) {
                for (int i = 1; i < NUM_MANUAL_LOADERS; i++) {
                    if (ImGui::MenuItem(dataSetNames.at(i).c_str())) {
                        selectedDataSetIndex = i;
                    }
                }

                if (dataSetInformationRoot) {
                    std::stack<std::pair<DataSetInformationPtr, size_t>> dataSetInformationStack;
                    dataSetInformationStack.push(std::make_pair(dataSetInformationRoot, 0));
                    while (!dataSetInformationStack.empty()) {
                        std::pair<DataSetInformationPtr, size_t> dataSetIdxPair = dataSetInformationStack.top();
                        DataSetInformationPtr dataSetInformationParent = dataSetIdxPair.first;
                        size_t idx = dataSetIdxPair.second;
                        dataSetInformationStack.pop();
                        while (idx < dataSetInformationParent->children.size()) {
                            DataSetInformationPtr dataSetInformationChild =
                                    dataSetInformationParent->children.at(idx);
                            if (dataSetInformationChild->type == DATA_SET_TYPE_NODE) {
                                if (ImGui::BeginMenu(dataSetInformationChild->name.c_str())) {
                                    dataSetInformationStack.push(std::make_pair(dataSetInformationRoot, idx + 1));
                                    dataSetInformationStack.push(std::make_pair(dataSetInformationChild, 0));
                                    break;
                                }
                            } else {
                                if (ImGui::MenuItem(dataSetInformationChild->name.c_str())) {
                                    selectedDataSetIndex = int(dataSetInformationChild->sequentialIndex);
                                    loadCloudDataSet(getSelectedDataSetFilename());
                                }
                            }
                            idx++;
                        }

                        if (idx == dataSetInformationParent->children.size() && !dataSetInformationStack.empty()) {
                            ImGui::EndMenu();
                        }
                    }
                }

                ImGui::EndMenu();
            }

            if (ImGui::MenuItem("Quit", "CTRL+Q")) {
                quit();
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Window")) {
            if (ImGui::MenuItem("Volumetric Path Tracer", nullptr, showRendererWindow)) {
                showRendererWindow = !showRendererWindow;
            }
            if (ImGui::MenuItem("FPS Overlay", nullptr, showFpsOverlay)) {
                showFpsOverlay = !showFpsOverlay;
            }
            if (ImGui::MenuItem("Coordinate Axes Overlay", nullptr, showCoordinateAxesOverlay)) {
                showCoordinateAxesOverlay = !showCoordinateAxesOverlay;
            }
            if (ImGui::MenuItem("Property Editor", nullptr, showPropertyEditor)) {
                showPropertyEditor = !showPropertyEditor;
            }
            if (ImGui::MenuItem("Checkpoint Window", nullptr, checkpointWindow.getShowWindow())) {
                checkpointWindow.setShowWindow(!checkpointWindow.getShowWindow());
            }
            ImGui::EndMenu();
        }

        //if (dataRequester.getIsProcessingRequest()) {
        //    ImGui::SetCursorPosX(ImGui::GetWindowContentRegionWidth() - ImGui::GetTextLineHeight());
        //    ImGui::ProgressSpinner(
        //            "##progress-spinner", -1.0f, -1.0f, 4.0f,
        //            ImVec4(0.1f, 0.5f, 1.0f, 1.0f));
        //}

        ImGui::EndMainMenuBar();
    }
}

void MainApp::renderGuiPropertyEditorBegin() {
    if (!useDockSpaceMode) {
        renderGuiFpsCounter();

        if (ImGui::Combo(
                "Data Set", &selectedDataSetIndex, dataSetNames.data(),
                int(dataSetNames.size()))) {
            if (selectedDataSetIndex >= NUM_MANUAL_LOADERS) {
                loadCloudDataSet(getSelectedDataSetFilename());
            }
        }

        //if (dataRequester.getIsProcessingRequest()) {
        //    ImGui::SameLine();
        //    ImGui::ProgressSpinner(
        //            "##progress-spinner", -1.0f, -1.0f, 4.0f,
        //            ImVec4(0.1, 0.5, 1.0, 1.0));
        //}


        if (selectedDataSetIndex == 0) {
            ImGui::InputText("##datasetfilenamelabel", &customDataSetFileName);
            ImGui::SameLine();
            if (ImGui::Button("Load File")) {
                loadCloudDataSet(getSelectedDataSetFilename());
            }
        }

        ImGui::Separator();
    }
}

void MainApp::renderGuiPropertyEditorCustomNodes() {
    if (propertyEditor.beginNode("Volumetric Path Tracer")) {
        volumetricPathTracingPass->renderGuiPropertyEditorNodes(propertyEditor);
        propertyEditor.endNode();
    }
}

void MainApp::update(float dt) {
    sgl::SciVisApp::update(dt);

    if (scheduledDockSpaceModeChange) {
        useDockSpaceMode = newDockSpaceMode;
        scheduledDockSpaceModeChange = false;
        if (useDockSpaceMode) {
            cameraHandle = dataView->camera;
        } else {
            cameraHandle = camera;
        }
        //device->waitGraphicsQueueIdle();
    }

    updateCameraFlight(cloudData.get() != nullptr, usesNewState);

    checkLoadingRequestFinished();

    ImGuiIO &io = ImGui::GetIO();
    if (!io.WantCaptureKeyboard || recording || focusedWindowIndex != -1) {
        moveCameraKeyboard(dt);
    }

    if (!io.WantCaptureMouse || mouseHoverWindowIndex != -1) {
        moveCameraMouse(dt);
    }
}

void MainApp::hasMoved() {
    volumetricPathTracingPass->onHasMoved();
}

void MainApp::onCameraReset() {
}


// --- Visualization pipeline ---

void MainApp::loadCloudDataSet(const std::string& fileName, bool blockingDataLoading) {
    if (fileName.empty()) {
        cloudData = CloudDataPtr();
        return;
    }
    currentlyLoadedDataSetIndex = selectedDataSetIndex;

    DataSetInformation selectedDataSetInformation;
    if (selectedDataSetIndex >= NUM_MANUAL_LOADERS && !dataSetInformationList.empty()) {
        selectedDataSetInformation = *dataSetInformationList.at(selectedDataSetIndex - NUM_MANUAL_LOADERS);
    } else {
        selectedDataSetInformation.filename = fileName;
    }

    glm::mat4 transformationMatrix = sgl::matrixIdentity();
    //glm::mat4* transformationMatrixPtr = nullptr;
    if (selectedDataSetInformation.hasCustomTransform) {
        transformationMatrix *= selectedDataSetInformation.transformMatrix;
        //transformationMatrixPtr = &transformationMatrix;
    }
    if (rotateModelBy90DegreeTurns != 0) {
        transformationMatrix *= glm::rotate(rotateModelBy90DegreeTurns * sgl::HALF_PI, modelRotationAxis);
        //transformationMatrixPtr = &transformationMatrix;
    }

    CloudDataPtr cloudData(new CloudData);

    if (blockingDataLoading) {
        //bool dataLoaded = cloudData->loadFromFile(fileName, selectedDataSetInformation, transformationMatrixPtr);
        bool dataLoaded = cloudData->loadFromFile(fileName);

        if (dataLoaded) {
            this->cloudData = cloudData;
            cloudData->setClearColor(clearColor);
            newMeshLoaded = true;
            //modelBoundingBox = cloudData->getModelBoundingBox();

            volumetricPathTracingPass->setCloudData(cloudData);
            volumetricPathTracingPass->setUseLinearRGB(useLinearRGB);
            reRender = true;

            const std::string& meshDescriptorName = fileName;
            checkpointWindow.onLoadDataSet(meshDescriptorName);

            if (true) {
                std::string cameraPathFilename =
                        saveDirectoryCameraPaths + sgl::FileUtils::get()->getPathAsList(meshDescriptorName).back()
                        + ".binpath";
                if (sgl::FileUtils::get()->exists(cameraPathFilename)) {
                    cameraPath.fromBinaryFile(cameraPathFilename);
                } else {
                    cameraPath.fromCirclePath(
                            modelBoundingBox, meshDescriptorName,
                            usePerformanceMeasurementMode
                            ? CAMERA_PATH_TIME_PERFORMANCE_MEASUREMENT : CAMERA_PATH_TIME_RECORDING,
                            usePerformanceMeasurementMode);
                }
            }
        }
    } else {
        //dataRequester.queueRequest(cloudData, fileName, selectedDataSetInformation, transformationMatrixPtr);
    }
}

void MainApp::checkLoadingRequestFinished() {
    CloudDataPtr cloudData;
    DataSetInformation loadedDataSetInformation;

    //if (!cloudData) {
    //    cloudData = dataRequester.getLoadedData(loadedDataSetInformation);
    //}

    if (cloudData) {
        this->cloudData = cloudData;
        cloudData->setClearColor(clearColor);
        newMeshLoaded = true;
        //modelBoundingBox = cloudData->getModelBoundingBox();

        std::string meshDescriptorName = cloudData->getFileName();
        checkpointWindow.onLoadDataSet(meshDescriptorName);

        if (true) {
            std::string cameraPathFilename =
                    saveDirectoryCameraPaths + sgl::FileUtils::get()->getPathAsList(meshDescriptorName).back()
                    + ".binpath";
            if (sgl::FileUtils::get()->exists(cameraPathFilename)) {
                cameraPath.fromBinaryFile(cameraPathFilename);
            } else {
                cameraPath.fromCirclePath(
                        modelBoundingBox, meshDescriptorName,
                        usePerformanceMeasurementMode
                        ? CAMERA_PATH_TIME_PERFORMANCE_MEASUREMENT : CAMERA_PATH_TIME_RECORDING,
                        usePerformanceMeasurementMode);
            }
        }
    }
}

void MainApp::reloadDataSet() {
    loadCloudDataSet(getSelectedDataSetFilename());
}
