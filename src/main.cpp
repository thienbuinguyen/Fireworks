#include <SDL2/SDL.h>
#include <GL/glew.h>
#include <SDL2/SDL_opengl.h>
#include <SDL2/SDL_image.h>
#include <iostream>
#include <string>
#include <fstream>
#include <sstream>
#include <stdio.h>
#include <vector>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <algorithm>
#include <iterator>

using namespace std;

const int SCREEN_WIDTH = 800;
const int SCREEN_HEIGHT = 600;
const int WORLD_WIDTH = 800;
const int WORLD_HEIGHT = 600;
const int NUM_OUTER_CIRCLE_VERTICES = 50; // vertices along the arc of each circle

SDL_Window *window = nullptr;
SDL_GLContext context = NULL;
GLuint programObj;
GLuint VAO, VBO; // vertex array object and vertex buffer objects

// simulation variables
const glm::vec3 GRAVITY(0.f, -200.f, 0.f);
const int NUM_FIREWORKS = 10;
const int MIN_PARTICLES = 30;
const int MAX_PARTICLES = 50;
const int NUM_TRAIL_PARTICLES = 15; // for rocket and explosion particles
const float MIN_SCALE = 1; // min scale of particles
const int SCALE_RANGE = 2; // range of the scale for particles

// rocket
const int MAX_INIT_X_VEL = 20;
const int MIN_INIT_X_VEL = -20;
const int MAX_INIT_Y_VEL = 500;
const int MIN_INIT_Y_VEL = 300;

// particles
const int MIN_MAGNITUDE = 20;
const int MAX_MAGNITUDE = 200;
const float EXPLOSION_LIFE_DECREASE_RATE = 0.5; 
const float TRAIL_MIN_DECREASE_RATE = 3; // min number of respawns per second
const float TRAIL_MAX_DECREASE_RATE = 6; // max number of respawns per second

// camera variables
glm::mat4 projection = glm::ortho(0.f, (float) SCREEN_WIDTH, 0.f, (float) SCREEN_HEIGHT, -1.f, 1.f);
glm::mat4 view = glm::lookAt(
        glm::vec3(0.f, 0.f, 1.f),
        glm::vec3(0.f, 0.f, 0.f),
        glm::vec3(0.f, 1.f, 0.f)
        );

// Base particle class for explosions and trails
struct Particle {
    glm::vec3 pos;
    glm::vec3 vel;
    glm::vec4 color;
    float life = 1.0;
    float scale;
    float lifeDecreaseRate;

    Particle(glm::vec3 pos, glm::vec3 vel, glm::vec4 color, float scale) : 
        pos(pos), vel(vel), color(color), scale(scale) {}

    virtual void update(float dt) {};

    void render() {
        glm::mat4 model(1.f);
        model = glm::translate(model, pos);
        model = glm::scale(model, glm::vec3(scale, scale, 1.f));
        glm::mat4 mvp = projection * view * model;

        glUniformMatrix4fv(glGetUniformLocation(programObj, "mvp"), 1, GL_FALSE, &mvp[0][0]);
        glUniform4fv(glGetUniformLocation(programObj, "fragColor"), 1, glm::value_ptr(color));
        glDrawArrays(GL_TRIANGLE_FAN, 0, NUM_OUTER_CIRCLE_VERTICES + 1);
    }
};

// Trailing particles
struct TrailParticle : public Particle {
    float lifeDecreaseRate;

    TrailParticle(glm::vec3 pos, glm::vec3 vel, glm::vec4 color, float scale, float lifeDecreaseRate) 
        : lifeDecreaseRate(lifeDecreaseRate), Particle(pos, vel, color, scale) {}

    virtual void update(float dt, glm::vec3 &sourceVel, float sourceLife) {
        vel = sourceVel;
        pos += life * vel * dt;
        if (life > sourceLife) life = sourceLife; // restrict alpha value of trailing particles
        color.w = life;
        life -= lifeDecreaseRate * dt;
    }
};

// Explosion particles
struct ExplosionParticle : public Particle {
    glm::vec3 origVel;
    vector<TrailParticle> trailParticles;

    ExplosionParticle(glm::vec3 pos, glm::vec3 vel, glm::vec4 color, float scale) 
        : Particle(pos, vel, color, scale), origVel(vel) {
            for (int i = 0; i < NUM_TRAIL_PARTICLES; ++i) {
                float lifeDecrease = (float) rand() / RAND_MAX * (TRAIL_MAX_DECREASE_RATE - TRAIL_MIN_DECREASE_RATE) + TRAIL_MIN_DECREASE_RATE;
                glm::vec3 particleVel = vel * 0.1f;
                trailParticles.push_back(TrailParticle(pos, particleVel, color, 1, lifeDecrease));
            }
        }

    // relocate a trailing particle to based on current location of the ExplosionParticle object
    void respawnTrailParticle(TrailParticle &p) {
        float random = ((rand() % 100) - 50) / 10.0f;
        p.life = 1.0f;
        p.pos = pos + random;
        p.vel = vel * 0.1f;
    }

    virtual void update(float dt) {
        for (auto &p : trailParticles) {
            p.update(dt, vel, life);
            if (p.life <= 0) respawnTrailParticle(p);
        }

        vel = life * origVel * dt; // decrease speed of the particle over time
        pos += vel;
        color.w = life;
        life -= EXPLOSION_LIFE_DECREASE_RATE * dt;
    }

    void render() {
        for (auto &p : trailParticles) p.render();
        this->Particle::render();
    }
};

// Firework class that maintains the "rocket" and all particles of the firework
struct Firework {
    glm::vec3 pos;
    glm::vec3 vel;
    glm::vec4 color;
    float scale;
    bool exploded = false;
    int numParticles;
    vector<TrailParticle> trailParticles;
    vector<ExplosionParticle> explosionParticles;

    Firework() {
        reset();
    }

    void respawnParticle(TrailParticle &p) {
        float random = ((rand() % 100) - 50) / 10.0f;
        p.life = 1.0f;
        p.pos = pos + random;
        p.vel = vel * ((float) rand() / RAND_MAX * 0.25f + 0.75f);
    }

    void randomiseColor() {
        // randomise rgb colors in range [0.25, 1.0]
        float r = ((float) rand() / RAND_MAX) * 0.75f + 0.25f;
        float g = ((float) rand() / RAND_MAX) * 0.75f + 0.25f;
        float b = ((float) rand() / RAND_MAX) * 0.75f + 0.25f;
        color = glm::vec4(r, g, b, 1.0f);
    }

    // Destroy exisiting particles and reset firework variables
    void reset() {
        for (auto &p : explosionParticles) p.trailParticles.clear();
        explosionParticles.clear();
        trailParticles.clear();
        exploded = false;

        numParticles = rand() % (MAX_PARTICLES - MIN_PARTICLES) + MIN_PARTICLES;
        pos = glm::vec3((float) (rand() % WORLD_WIDTH), 0.f, 0.f);
        vel = glm::vec3(rand() % (MAX_INIT_X_VEL - MIN_INIT_X_VEL) + MIN_INIT_X_VEL, rand() % (MAX_INIT_Y_VEL - MIN_INIT_Y_VEL) + MIN_INIT_Y_VEL, 0.f);
        scale = rand() % SCALE_RANGE + MIN_SCALE;

        randomiseColor();

        for (int i = 0; i < NUM_TRAIL_PARTICLES; ++i) {
            float lifeDecrease = (float) rand() / RAND_MAX * (TRAIL_MAX_DECREASE_RATE - TRAIL_MIN_DECREASE_RATE) + TRAIL_MIN_DECREASE_RATE;
            glm::vec3 particleVel = vel * ((float) rand() / RAND_MAX * 0.25f + 0.75f);
            trailParticles.push_back(TrailParticle(pos, particleVel, color, 1, lifeDecrease));
        }
    }

    void update(float dt) {
        if (exploded)  // update all explosion particles
            for (auto &p : explosionParticles) { 
                p.update(dt);
                if (p.life <= 0) reset();
            }
        else { // update the rocket
            vel += GRAVITY * dt;
            pos += vel * dt;

            for (auto &p : trailParticles) { 
                p.update(dt, vel, 1.0f);
                if (p.life <= 0) respawnParticle(p);
            }

            if (vel.y < 0) {
                exploded = true;
                trailParticles.clear();

                float theta =  M_PI * 2 / (float) NUM_OUTER_CIRCLE_VERTICES;

                // create explosion particles
                for (int i = 0; i < numParticles; ++i) {
                    float randTheta = rand() % NUM_OUTER_CIRCLE_VERTICES * theta;   // randomise the direction of the particle
                    glm::vec3 particleVel = glm::vec3(glm::cos(randTheta), glm::sin(randTheta), 0.f);
                    particleVel *= rand() % (MAX_MAGNITUDE - MIN_MAGNITUDE) + MIN_MAGNITUDE; // randomise the magnitude of the particle's speed
                    explosionParticles.push_back(ExplosionParticle(pos, particleVel, color, rand() % SCALE_RANGE + MIN_SCALE));
                }
            }
        }
    }

    void render() {
        if (!exploded) {
            for (auto &p : trailParticles) p.render();
            glm::mat4 model(1.f);
            model = glm::translate(model, pos);
            model = glm::scale(model, glm::vec3(scale, scale, 1.f));
            glm::mat4 mvp = projection * view * model;

            glUniformMatrix4fv(glGetUniformLocation(programObj, "mvp"), 1, GL_FALSE, &mvp[0][0]);
            glUniform4fv(glGetUniformLocation(programObj, "fragColor"), 1, glm::value_ptr(color));
            glDrawArrays(GL_TRIANGLE_FAN, 0, NUM_OUTER_CIRCLE_VERTICES + 1);
        } else {
            for (auto &p : explosionParticles) p.render();
        }
    }
};

bool init();
bool initGL();
void initFireworks();
void render();
void update(float dt);
void setupGLBuffers();
void close();

vector<Firework> fireworks;

bool init() {
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        cout << "Failed to initialize SDL" << endl;
        return false;
    }

    int imgFlags = IMG_INIT_PNG;
    if (!(IMG_Init(imgFlags) & imgFlags)) {
        cout << "Failed to initialize SDL_image" << endl;
        return false;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    window = SDL_CreateWindow("Fireworks", SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED, SCREEN_WIDTH, SCREEN_HEIGHT,
            SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN);

    if (window == nullptr) {
        cout << "Failed to create window" << endl;
        return false;
    }

    context = SDL_GL_CreateContext(window);
    if (context == NULL) {
        cout << "Failed to create context" << endl;
        return false;
    }

    glewExperimental = GL_TRUE;
    GLenum glewError = glewInit();
    if (glewError != GLEW_OK) {
        cout << "Failed to initialize GLEW" << endl;
        return false;
    }

    if (!initGL()) {
        cout << "Failed to initialize OpenGL and shaders" << endl;
        return false;
    }
    return true;
}

// read a file and return its contents as a string
string fileToString(const string& file) {
    ifstream ifs(file);
    stringstream ss;

    while (ifs >> ss.rdbuf());
    return ss.str();
}

void printShaderLog(GLuint shader) {
    if (glIsShader(shader) ) {
        int infoLogLength = 0;
        int maxLength = infoLogLength;

        glGetShaderiv( shader, GL_INFO_LOG_LENGTH, &maxLength );
        char* infoLog = new char[ maxLength ];

        glGetShaderInfoLog( shader, maxLength, &infoLogLength, infoLog );
        if (infoLogLength > 0) {
            printf("%s\n", &(infoLog[0]));
        }

        delete[] infoLog;
    } else {
        cout << to_string(shader) << " is not a shader" << endl;
    }
}

bool initGL() {
    glEnable(GL_TEXTURE_2D);
    GLint compileSuccess;
    string temp;

    GLuint vShader = glCreateShader(GL_VERTEX_SHADER);
    temp = fileToString("./shaders/vertex.glsl");
    const char* vShaderSource = temp.c_str();
    glShaderSource(vShader, 1, &vShaderSource, NULL);
    glCompileShader(vShader);

    glGetShaderiv(vShader, GL_COMPILE_STATUS, &compileSuccess);
    if (!compileSuccess) {
        cout << "Failed to compile vertex shader" << endl;
        printShaderLog(vShader);
        return false;
    }

    GLuint fShader = glCreateShader(GL_FRAGMENT_SHADER);
    temp = fileToString("./shaders/fragment.glsl");
    const char* fShaderSource = temp.c_str();
    glShaderSource(fShader, 1, &fShaderSource, NULL);
    glCompileShader(fShader);

    glGetShaderiv(fShader, GL_COMPILE_STATUS, &compileSuccess);
    if (!compileSuccess) {
        cout << "Failed to compile fragment shader" << endl;
        printShaderLog(fShader);
        return false;
    }

    programObj = glCreateProgram();
    glAttachShader(programObj, vShader);
    glAttachShader(programObj, fShader);
    glLinkProgram(programObj);

    // flag shaders for deletion on program delete
    glDeleteShader(vShader);
    glDeleteShader(fShader);
    return true;
}

// create and initialise the vector of fireworks
void initFireworks() {
    for (int i = 0; i < NUM_FIREWORKS; ++i) {
        fireworks.push_back(Firework());
    }
}

void setupGLBuffers() {
    glCreateVertexArrays(1, &VAO);
    glBindVertexArray(VAO);

    // calculate vertices for a circle
    float vertices[(NUM_OUTER_CIRCLE_VERTICES + 1) * 3];

    // set last vertex to center of circle
    int len = sizeof(vertices) / sizeof(*vertices);
    vertices[len - 1] = 0;
    vertices[len - 2] = 0;
    vertices[len - 3] = 0;

    float theta = M_PI * 2 / (float) NUM_OUTER_CIRCLE_VERTICES;
    float cosine = glm::cos(theta);
    float sine = glm::sin(theta);

    float x = 1;
    float y = 0;

    for (int i = 0; i < NUM_OUTER_CIRCLE_VERTICES; ++i) {
        float temp = x;
        x = x * cosine - y * sine;
        y = temp * sine + y * cosine;

        vertices[i * 3] = x;
        vertices[i * 3 + 1] = y;
        vertices[i * 3 + 2] = 0; // let z axis be 0
    }

    // create vertex and color buffers
    glCreateBuffers(1, &VBO);
    glBindBuffer(GL_ARRAY_BUFFER, VBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 0, NULL);
}

// update all fireworks in the world
void update(float dt) {
    for (auto & firework : fireworks) firework.update(dt);
}

void render() {
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(programObj);
    glBindVertexArray(VAO);

    glm::mat4 projection = glm::ortho(0.f, (float) SCREEN_WIDTH, 0.f, (float) SCREEN_HEIGHT, -1.f, 1.f);
    glm::mat4 view = glm::lookAt(
            glm::vec3(0.f, 0.f, 1.f),
            glm::vec3(0.f, 0.f, 0.f),
            glm::vec3(0.f, 1.f, 0.f)
            );

    for (auto firework : fireworks) firework.render();

    glBindVertexArray(0);
    glUseProgram(0);
}

void close() {
    cout << "Shutting down..." << endl;
    SDL_DestroyWindow(window);
    window = nullptr;

    glDeleteProgram(programObj);
    glDeleteVertexArrays(1, &VAO);
    glDeleteBuffers(1, &VBO);

    SDL_Quit();
}


int main(int argc, char ** argv) {
    if (init()) {
        srand(time(0));
        glEnable(GL_BLEND);
        glBlendFunc(GL_SRC_ALPHA, GL_ONE);
        glClearColor(0.f, 0.f, 0.f, 1.0f);

        bool quit = false;
        SDL_Event e;
        Uint32 ticks, prevTicks, prevFrameTicks;
        short frames = 0;
        prevTicks = SDL_GetTicks();
        prevFrameTicks = prevTicks;

        int texWidth, texHeight;
        setupGLBuffers();
        initFireworks();

        SDL_StartTextInput();
        while (!quit) {
            while (SDL_PollEvent(&e) != 0) {
                if (e.type == SDL_QUIT) quit = true;
            }
            // performance measuring
            frames++;
            ticks = SDL_GetTicks();
            float deltaTime = (ticks - prevFrameTicks) / 1000.0; // change in time (seconds)
            prevFrameTicks = ticks;

            if (ticks - prevTicks >= 1000) { // for every second
                cout << to_string(1000.0 / frames) << " ms/frame" << endl;
                frames = 0;
                prevTicks = ticks;
            }

            update(deltaTime);
            render();

            SDL_GL_SwapWindow(window);
        }
        SDL_StopTextInput();
    }

    close();
    return 0;
}
