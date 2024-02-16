/*
 * Copyright (C) 2020 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <filamentapp/Config.h>
#include <filamentapp/FilamentApp.h>
#include <filamentapp/IBL.h>

#include <filament/Camera.h>

#include <filament/Engine.h>
#include <filament/Scene.h>
#include <filament/Skybox.h>
#include <filament/TransformManager.h>
#include <filament/View.h>
#include <filament/VertexBuffer.h>
#include <filament/IndexBuffer.h>
#include <filament/RenderableManager.h>
#include <filament/TextureSampler.h>
#include <filament/Material.h>

#include <gltfio/AssetLoader.h>
#include <gltfio/FilamentAsset.h>
#include <gltfio/ResourceLoader.h>
#include <gltfio/TextureProvider.h>

#include <viewer/ViewerGui.h>

#include <camutils/Manipulator.h>

#include <utils/NameComponentManager.h>

#include <iostream>
#include <fstream>
#include <string>

#include <math/mat4.h>
#include <mathio/ostream.h>
#include <stb_image.h>
#include <imgui.h>

#include "generated/resources/gltf_wind.h"

#include "generated/resources/resources.h"

#include "materials/uberarchive.h"

using namespace filament;
using namespace filament::math;
using namespace filament::viewer;

using namespace filament::gltfio;
using namespace utils;

using MinFilter = TextureSampler::MinFilter;
using MagFilter = TextureSampler::MagFilter;


enum MaterialSource {
    JITSHADER,
    UBERSHADER,
};


struct Vertex {
    filament::math::float2 position;
    filament::math::float2 uv;
};

static const Vertex QUAD_VERTICES[4] = {
    {{-1, -1}, {0, 0}},
    {{ 1, -1}, {1, 0}},
    {{-1,  1}, {0, 1}},
    {{ 1,  1}, {1, 1}},
};

static constexpr uint16_t QUAD_INDICES[6] = {
    0, 1, 2,
    3, 2, 1,
};

const double HalfWidth = 5;
const double HalfHeight = 5;

struct App {
    Engine* engine;
    ViewerGui* viewer;
    Config config;
    AssetLoader* loader;
    FilamentAsset* asset = nullptr;
    NameComponentManager* names;
    MaterialProvider* materials;
    MaterialSource materialSource = JITSHADER;
    ResourceLoader* resourceLoader = nullptr;
    gltfio::TextureProvider* stbDecoder = nullptr;
    std::vector<FilamentInstance*> instances;
    // quad
    Texture* quadTex;
    VertexBuffer* quadVertexBuffer;
    IndexBuffer* quadIndexBuffer;

    Material* quadMaterial;
    MaterialInstance* quadMaterialInstance;
    Entity renderable;
};


static std::ifstream::pos_type getFileSize(const char* filename) {
    std::ifstream in(filename, std::ifstream::ate | std::ifstream::binary);
    return in.tellg();
}


Camera *cam;


int main(int argc, char** argv) {
    App app;
    app.config.title = "Wind GLTF";
    app.config.backend = Engine::Backend::OPENGL;
    
    app.instances.resize(1);
    app.materialSource = UBERSHADER;

    utils::Path filename;
    auto loadAsset = [&app](utils::Path filename) {
        // Peek at the file size to allow pre-allocation.
        long contentSize = static_cast<long>(getFileSize(filename.c_str()));
        if (contentSize <= 0) {
            std::cerr << "Unable to open " << filename << std::endl;
            exit(1);
        }

        // Consume the glTF file.
        std::ifstream in(filename.c_str(), std::ifstream::binary | std::ifstream::in);
        std::vector<uint8_t> buffer(static_cast<unsigned long>(contentSize));
        if (!in.read((char*) buffer.data(), contentSize)) {
            std::cerr << "Unable to read " << filename << std::endl;
            exit(1);
        }

        // Parse the glTF file and create Filament entities.
        app.asset = app.loader->createInstancedAsset(buffer.data(), buffer.size(),
                app.instances.data(), app.instances.size());
        Aabb aabb = app.asset->getBoundingBox();
        std::cout << "center: " << aabb.center() << std::endl;
        buffer.clear();
        buffer.shrink_to_fit();

        if (!app.asset) {
            std::cerr << "Unable to parse " << filename << std::endl;
            exit(1);
        }
    };

    auto loadResources = [&app] (utils::Path filename) {
        // Load external textures and buffers.
        std::string gltfPath = filename.getAbsolutePath();
        std::cout << "GLTF PATH: " << gltfPath << std::endl;
        ResourceConfiguration configuration;
        configuration.engine = app.engine;
        configuration.gltfPath = gltfPath.c_str();
        configuration.normalizeSkinningWeights = true;
        if (!app.resourceLoader) {
            app.resourceLoader = new gltfio::ResourceLoader(configuration);
            app.stbDecoder = createStbProvider(app.engine);
            app.resourceLoader->addTextureProvider("image/png", app.stbDecoder);
        }

        if (!app.resourceLoader->asyncBeginLoad(app.asset)) {
            std::cerr << "Unable to start loading resources for " << filename << std::endl;
            exit(1);
        }

        // auto ibl = FilamentApp::get().getIBL();
        // if (ibl) {
        //     app.viewer->setIndirectLight(ibl->getIndirectLight(), ibl->getSphericalHarmonics());
        // }
    };

    auto quadSetup = [&app](Engine* engine, View* view, Scene *scene) {
        Path path = FilamentApp::getRootAssetsPath() + "hvac_panel_front_bg.png";
        if (!path.exists()) {
            std::cerr << "Background texture " << path << " does not exist" << std::endl;
            exit(1);
        }
        int w, h, n;
        unsigned char* data = stbi_load(path.c_str(), &w, &h, &n, 4);
        if (data == nullptr) {
            std::cerr << "The texture " << path << " could not be loaded" << std::endl;
            exit(1);
        }
        std::cout << "Loaded texture: " << w << "x" << h << std::endl;
        Texture::PixelBufferDescriptor buffer(data, size_t(w * h * 4),
                Texture::Format::RGBA, Texture::Type::UBYTE,
                (Texture::PixelBufferDescriptor::Callback) &stbi_image_free);
        app.quadTex = Texture::Builder()
                .width(uint32_t(w))
                .height(uint32_t(h))
                .levels(1)
                .sampler(Texture::Sampler::SAMPLER_2D)
                .format(Texture::InternalFormat::RGBA8)
                .build(*engine);
        app.quadTex->setImage(*engine, 0, std::move(buffer));
        TextureSampler sampler(MinFilter::LINEAR, MagFilter::LINEAR);

        app.quadVertexBuffer = VertexBuffer::Builder()
                .vertexCount(4)
                .bufferCount(1)
                .attribute(VertexAttribute::POSITION, 0, VertexBuffer::AttributeType::FLOAT2, 0, 16)
                .attribute(VertexAttribute::UV0, 0, VertexBuffer::AttributeType::FLOAT2, 8, 16)
                .build(*engine);
        app.quadVertexBuffer->setBufferAt(*engine, 0,
                VertexBuffer::BufferDescriptor(QUAD_VERTICES, 64, nullptr));
        app.quadIndexBuffer = IndexBuffer::Builder()
                .indexCount(6)
                .bufferType(IndexBuffer::IndexType::USHORT)
                .build(*engine);
        app.quadIndexBuffer->setBuffer(*engine,
                IndexBuffer::BufferDescriptor(QUAD_INDICES, 12, nullptr));
        app.quadMaterial = Material::Builder()
                .package(RESOURCES_BAKEDTEXTURE_DATA, RESOURCES_BAKEDTEXTURE_SIZE)
                .build(*engine);
        app.quadMaterialInstance = app.quadMaterial->createInstance();
        app.quadMaterialInstance->setParameter("albedo", app.quadTex, sampler);
        app.renderable = EntityManager::get().create();
        RenderableManager::Builder(1)
                .boundingBox({{ -1, -1, -1 }, { 1, 1, -1 }})
                .material(0, app.quadMaterialInstance)
                .geometry(
                    0, RenderableManager::PrimitiveType::TRIANGLES,
                    app.quadVertexBuffer, app.quadIndexBuffer, 0, 6
                )
                .culling(false)
                .receiveShadows(false)
                .castShadows(false)
                .build(*engine, app.renderable);
        // scene->addEntity(app.renderable);
    };

    auto setup = [&](Engine* engine, View* view, Scene* scene) {
        app.engine = engine;
        app.names = new NameComponentManager(EntityManager::get());
        app.viewer = new ViewerGui(engine, scene, view);

        app.materials = (app.materialSource == JITSHADER) ?
                createJitShaderProvider(engine) :
                createUbershaderProvider(engine, UBERARCHIVE_DEFAULT_DATA, UBERARCHIVE_DEFAULT_SIZE);
        // engine->enableAccurateTranslations();

        app.loader = AssetLoader::create({engine, app.materials, app.names });

        cam = engine->createCamera(utils::EntityManager::get().create());
        view->setCamera(cam);
        cam->setProjection(Camera::Projection::ORTHO, 
            -HalfWidth, HalfWidth, -HalfHeight, HalfHeight, -1000.0, 1000.0);
        auto materialCount = app.loader->getMaterialsCount();
        std::cout << "material count: " << materialCount << std::endl;

        if (filename.isEmpty()) {
            std::cout << "file empty load default glb" << std::endl;
            app.asset = app.loader->createInstancedAsset(
                    GLTF_WIND_WIND_AIR_EFFECT_DATA, GLTF_WIND_WIND_AIR_EFFECT_SIZE,
                    app.instances.data(), app.instances.size());

        } else {
            loadAsset(filename);
        }

        loadResources(filename);
        FilamentInstance* instance = app.instances[0];
        std::cout << "entity count: " << instance->getEntityCount() << std::endl;
        instance->recomputeBoundingBoxes();
        auto aabb = instance->getBoundingBox();
        std::cout << "aabb center: " << aabb.center() << std::endl;
        app.viewer->setAsset(app.asset, instance);
        // auto& tcm = engine->getTransformManager();
        // auto transformRoot = tcm.getInstance(instance->getRoot());
        // tcm.setTransform(transformRoot, mat4f::rotation(-90.0 * M_PI / 180.0, float3{ 0, 1, 0 }));
    };


    auto cleanup = [&app](Engine* engine, View*, Scene*) {
        app.loader->destroyAsset(app.asset);
        app.materials->destroyMaterials();

        delete app.viewer;
        delete app.materials;
        delete app.names;
        delete app.resourceLoader;
        delete app.stbDecoder;

        AssetLoader::destroy(&app.loader);
    };

    auto animate = [&app](Engine* engine, View* view, double now) {
        app.resourceLoader->asyncUpdateLoad();
        app.viewer->updateRootTransform();
        app.viewer->populateScene();
        app.viewer->applyAnimation(now);
        // FilamentInstance* instance = app.instances[0];
        // auto& tcm = engine->getTransformManager();
        // auto transformRoot = tcm.getInstance(instance->getRoot());
        // tcm.setTransform(transformRoot, mat4f::rotation(now, float3{ 0, 1, 0 }));
    };

    auto gui = [&app](Engine* engine, View* view) { };

    auto resize = [&app](Engine*, View* view) {
        // Camera& camera = view->getCamera();
        // const Viewport& vp = view->getViewport();
        // double const aspectRatio = (double) vp.width / vp.height;
        // camera.setScaling({1.0 / aspectRatio, 1.0 });
    };

    auto preRender = [&app](Engine* engine, View* view, Scene* scene, Renderer* renderer) { 
     
    };

    FilamentApp& filamentApp = FilamentApp::get();
    filamentApp.animate(animate);
    // filamentApp.resize(resize);

    filamentApp.run(app.config, setup, cleanup, gui, preRender);

    return 0;
}
