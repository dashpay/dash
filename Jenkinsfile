node {
  def targets = [
    'win32',
    'win64',
    'linux32',
    'linux64',
    'linux64_nowallet',
    'linux64_release',
    'mac',
  ]

  def tasks = [:]
  for(int i = 0; i < targets.size(); i++) {
    def target = targets[i]

    def env = [
      "BUILD_TARGET=${target}",
      "PULL_REQUEST=false",
      "JOB_NUMBER=${env.BUILD_NUMBER}",
      "USE_VOLUMES_FOR_CACHE=true",
    ]

    tasks["${target}"] = {
      withEnv(env) {
        stage("${target}/checkout") {
          node {
            checkout scm
          }
        }

        stage("${target}/build") {
          node {
            try {
              sh './ci/build_builder.sh'
              sh './ci/make_volumes.sh'
              sh './ci/build_depends.sh'
              sh './ci/build_src.sh'
            } catch(err) {
              sh './ci/cleanup_docker.sh'
              throw err
            }
          }
        }
        stage("${target}/test") {
          node {
            try {
              sh './ci/test_unittests.sh'
              sh './ci/test_integrationtests.sh'
              sh './ci/cleanup_docker.sh'
            } catch(err) {
              sh './ci/cleanup_docker.sh'
            }
          }
        }
        stage("${target}/cleanup") {
          node {
            sh './ci/cleanup_docker.sh'
          }
        }
      }
    }
  }
  stage ("Matrix") {
    parallel tasks
  }
}
